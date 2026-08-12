#include "radio_page.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include "esp_audio_simple_dec.h"
#include "impl/esp_mp3_dec.h"

#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "RadioPage"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr lv_coord_t kBarW = 7;
constexpr lv_coord_t kBarGap = 2;
constexpr lv_coord_t kPlotTop = 62;
constexpr lv_coord_t kPlotH = 48;
constexpr int kVisSamples = 512;
constexpr int kBandCount = 24;

// Plain HTTP MPEG1 Layer III only. No HLS, no TS, no TLS: mbedtls' handshake needs
// ~8-10KB of the *calling task's* stack, which is what a no-PSRAM S3 cannot spare
// next to an MP3 decoder. Both URLs verified as 64kbps 44100Hz joint stereo.
struct Station {
    const char* short_name;
    const char* title;
    const char* url;
};

constexpr Station kStations[] = {
    {"News", "CNR1 Voice of China", "http://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"Music", "FM974 Beijing Music", "http://lhttp.qtfm.cn/live/332/64k.mp3"},
};
constexpr int kStationCount = static_cast<int>(sizeof(kStations) / sizeof(kStations[0]));
constexpr int kDefaultStation = 1;

// This board has no PSRAM and idles at ~44KB free internal SRAM. Disabling the wake
// word only stops its task, it does not release the AFE, so the whole radio path has
// to fit in that ~44KB alongside the MP3 decoder (~19KB) and lwIP's DNS/socket needs.
// Every size below is the measured worst case, not a rounded-up guess.
constexpr int kHttpChunk = 512;
constexpr int kInBufBytes = 2 * 1024;   // max MPEG1-L3 frame is 1044B, so one always fits
constexpr int kPcmOutInit = 5 * 1024;   // 1152 samples * 2ch * 2B = 4608
constexpr int kPcmOutMax = 8 * 1024;
// Measured peak usage of this task is ~1.8KB (8192 stack reported 6392 free), so
// 5KB keeps a ~2.5x margin while handing the rest to the decoder.
constexpr int kTaskStack = 5120;
constexpr int kResampleChunk = 512;     // stack-resident, keeps another vector off the heap
constexpr size_t kMinHeapToStream = 12 * 1024;
constexpr int kTaskPrio = 3;
// LVGL runs at priority 1 pinned to core 1 (see lcd_display.cc task_affinity=1), so a
// higher-priority streaming task on core 1 starves it outright and trips the task
// watchdog on taskLVGL. Keep this off core 1.
constexpr int kTaskCore = 0;
constexpr int kHttpTimeoutMs = 6000;
// Join must outlast a blocked esp_http_client open/read (timeout_ms above). With
// AbortActiveHttp the wait is usually ms; this is the pathological ceiling.
constexpr int kStreamJoinMs = kHttpTimeoutMs + 2000;
constexpr int kDefaultVolume = 85;
constexpr uint32_t kNoPcmAbortBytes = 48 * 1024;

size_t InternalHeapFree() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t InternalHeapLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

const char* AudioErrName(esp_audio_err_t err) {
    switch (err) {
        case ESP_AUDIO_ERR_OK:
            return "OK";
        case ESP_AUDIO_ERR_CONTINUE:
            return "CONTINUE";
        case ESP_AUDIO_ERR_FAIL:
            return "FAIL";
        case ESP_AUDIO_ERR_MEM_LACK:
            return "MEM_LACK";
        case ESP_AUDIO_ERR_DATA_LACK:
            return "DATA_LACK";
        case ESP_AUDIO_ERR_HEADER_PARSE:
            return "HEADER_PARSE";
        case ESP_AUDIO_ERR_INVALID_PARAMETER:
            return "INVALID_PARAMETER";
        case ESP_AUDIO_ERR_ALREADY_EXIST:
            return "ALREADY_EXIST";
        case ESP_AUDIO_ERR_NOT_SUPPORT:
            return "NOT_SUPPORT";
        case ESP_AUDIO_ERR_BUFF_NOT_ENOUGH:
            return "BUFF_NOT_ENOUGH";
        case ESP_AUDIO_ERR_NOT_FOUND:
            return "NOT_FOUND";
        default:
            return "UNKNOWN";
    }
}

constexpr int kToneMs = 350;
constexpr int kToneHz = 440;

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

struct StreamCtx {
    RadioPage* page = nullptr;
    esp_audio_simple_dec_handle_t dec = nullptr;
    std::vector<uint8_t> pcm;
    std::vector<int16_t> resample;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    float ema_level = 0.0f;
    int16_t vis_buf[kVisSamples];
    int vis_fill = 0;
    uint32_t pcm_chunks = 0;
    uint32_t bytes_in = 0;
    uint32_t frames_ok = 0;
    uint32_t frames_bad = 0;
    uint32_t info_misses = 0;
    uint32_t consec_bad = 0;
    bool format_rejected = false;
    double resample_pos = 0.0;
};

void PushVisSample(StreamCtx* ctx, int16_t sample) {
    if (ctx == nullptr || ctx->vis_fill >= kVisSamples) {
        return;
    }
    ctx->vis_buf[ctx->vis_fill++] = sample;
}

void NotifyBandsFromPcm(RadioPage* page, const int16_t* samples, int count) {
    if (page == nullptr || samples == nullptr || count <= 0) {
        return;
    }
    const int seg = count / kBandCount;
    if (seg <= 0) {
        return;
    }

    float energies[kBandCount];
    for (int b = 0; b < kBandCount; ++b) {
        double sum = 0;
        double diff_sum = 0;
        int16_t prev = 0;
        const int start = b * seg;
        const int end = (b == kBandCount - 1) ? count : (start + seg);
        for (int i = start; i < end; ++i) {
            const int16_t s = samples[i];
            sum += static_cast<double>(s) * static_cast<double>(s);
            const int d = static_cast<int>(s) - static_cast<int>(prev);
            diff_sum += static_cast<double>(d) * static_cast<double>(d);
            prev = s;
        }
        const int n = end - start;
        const float rms = static_cast<float>(std::sqrt(sum / n));
        const float hif = static_cast<float>(std::sqrt(diff_sum / n));
        const float w = 0.55f + 0.45f * (static_cast<float>(b) / (kBandCount - 1));
        energies[b] = rms * (1.0f - w) + hif * w;
    }
    page->NotifyBandLevels(energies, kBandCount);
}

void FlushVisBuffer(StreamCtx* ctx) {
    if (ctx == nullptr || ctx->vis_fill <= 0 || ctx->page == nullptr) {
        return;
    }
    NotifyBandsFromPcm(ctx->page, ctx->vis_buf, ctx->vis_fill);
    ctx->vis_fill = 0;
}

void FeedPcmToSpeaker(StreamCtx* ctx, const uint8_t* data, uint32_t bytes) {
    if (data == nullptr || bytes < 4 || ctx == nullptr || ctx->page == nullptr) {
        return;
    }
    if (!ctx->page->IsStreamRunning() || ctx->page->IsUserPaused() || ctx->format_rejected) {
        return;
    }
    // Never guess the PCM format. A wrong rate/channel/width guess is exactly what
    // turns the speaker into static, so if the decoder cannot state its output
    // format we stay silent instead of shipping bytes we cannot interpret.
    if (ctx->sample_rate == 0 || ctx->channels == 0) {
        esp_audio_simple_dec_info_t info{};
        if (esp_audio_simple_dec_get_info(ctx->dec, &info) != ESP_AUDIO_ERR_OK ||
            info.sample_rate == 0 || info.channel == 0) {
            if (++ctx->info_misses == 1 || (ctx->info_misses % 100) == 0) {
                ESP_LOGW(TAG, "decoder info not ready (%u) - holding output",
                         (unsigned)ctx->info_misses);
            }
            return;
        }
        if (info.bits_per_sample != 16) {
            ESP_LOGE(TAG, "unsupported bits_per_sample=%u; refusing to play",
                     (unsigned)info.bits_per_sample);
            ctx->format_rejected = true;
            ctx->page->SetStatusHint("bad pcm format");
            return;
        }
        ctx->sample_rate = info.sample_rate;
        ctx->channels = info.channel;
        auto* c = Board::GetInstance().GetAudioCodec();
        ESP_LOGI(TAG, "decoded PCM: %uHz %uch %ubit -> codec %dHz mono", (unsigned)info.sample_rate,
                 (unsigned)info.channel, (unsigned)info.bits_per_sample,
                 c != nullptr ? c->output_sample_rate() : -1);
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    // Output stays powered because AudioService holds it via SetExternalPlaybackActive().
    // Toggling EnableOutput() from here would race the audio-power esp_timer and the
    // AudioService output task inside esp_codec_dev_write().
    Application::GetInstance().GetAudioService().NotifyOutputActivity();

    const int ch = ctx->channels > 0 ? ctx->channels : 1;
    const int in_frames = static_cast<int>(bytes / sizeof(int16_t) / ch);
    if (in_frames <= 0) {
        return;
    }
    const int out_rate = codec->output_sample_rate();
    const int src_rate = static_cast<int>(ctx->sample_rate);
    if (out_rate <= 0 || src_rate <= 0) {
        return;
    }

    const int16_t* in = reinterpret_cast<const int16_t*>(data);
    const double step = static_cast<double>(src_rate) / static_cast<double>(out_rate);

    // Downmix to mono and rate-convert into one reusable buffer, flushing whenever
    // it fills. It is sized once at startup: allocating per frame (38 times a
    // second) would fragment a heap that is already nearly full.
    std::vector<int16_t>& out = ctx->resample;
    int n_out = 0;
    double energy = 0;
    int energy_n = 0;
    // resample_pos carries the fractional phase across frames, so restarting the
    // ratio at every frame boundary cannot inject a periodic click.
    for (double pos = ctx->resample_pos; pos < in_frames; pos += step) {
        const int i = static_cast<int>(pos);
        int32_t sample;
        if (ch >= 2) {
            sample = (static_cast<int32_t>(in[i * ch]) + static_cast<int32_t>(in[i * ch + 1])) / 2;
        } else {
            sample = in[i];
        }
        const int16_t mono = static_cast<int16_t>(sample);
        out[n_out++] = mono;
        PushVisSample(ctx, mono);
        energy += static_cast<double>(mono) * static_cast<double>(mono);
        ++energy_n;
        ctx->resample_pos = pos + step;
        if (n_out == kResampleChunk) {
            codec->OutputData(out);  // already exactly kResampleChunk long
            ++ctx->pcm_chunks;
            n_out = 0;
        }
    }
    ctx->resample_pos -= in_frames;
    if (ctx->resample_pos < 0) {
        ctx->resample_pos = 0;
    }
    if (n_out > 0) {
        // resize down then back up reuses the same allocation (capacity is kept).
        out.resize(n_out);
        codec->OutputData(out);
        out.resize(kResampleChunk);
        ++ctx->pcm_chunks;
    }

    if (ctx->pcm_chunks > 0) {
        ctx->page->NotifyPlaying();
        if (ctx->pcm_chunks == 1 || (ctx->pcm_chunks % 200) == 0) {
            ESP_LOGI(TAG, "pcm chunks=%u %u->%uHz ch=%d vol=%d heap=%u stack_free=%u",
                     (unsigned)ctx->pcm_chunks, (unsigned)src_rate, (unsigned)out_rate, ch,
                     codec->output_volume(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
    }
    if (ctx->vis_fill >= kVisSamples) {
        FlushVisBuffer(ctx);
    }
    if (energy_n > 0) {
        const float rms = static_cast<float>(std::sqrt(energy / energy_n) / 32768.0);
        ctx->ema_level = ctx->ema_level * 0.6f + rms * 0.4f;
        ctx->page->NotifyLevel(ctx->ema_level);
    }
}

// Decode every whole frame sitting in buf[0..fill). Returns the number of bytes
// consumed; the caller keeps the remainder for the next socket read.
// Feed the simple decoder the way its API intends: hand it whatever arrived and let
// its parser find the frames, advancing by however much it reports consuming. The
// earlier hand-rolled frame splitter fought the parser and desynced it.
int DecodeBuffered(StreamCtx* ctx, uint8_t* buf, int fill) {
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = buf;
    raw.len = static_cast<uint32_t>(fill);
    raw.eos = false;
    raw.consumed = 0;

    int off = 0;
    while (raw.len > 0) {
        if (ctx->page != nullptr && !ctx->page->IsStreamRunning()) {
            break;
        }

        bool had_error = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (ctx->pcm.size() < kPcmOutInit) {
                ctx->pcm.resize(kPcmOutInit);
            }
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = ctx->pcm.data();
            frame.len = static_cast<uint32_t>(ctx->pcm.size());

            const esp_audio_err_t ret = esp_audio_simple_dec_process(ctx->dec, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint32_t need = frame.needed_size > 0 ? frame.needed_size
                                                      : (uint32_t)(ctx->pcm.size() * 2);
                if (need > kPcmOutMax) {
                    need = kPcmOutMax;
                }
                if (need <= ctx->pcm.size()) {
                    break;
                }
                ctx->pcm.resize(need);
                continue;  // retry the same frame with a bigger sink
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                had_error = true;
                ++ctx->frames_bad;
                if (ret == ESP_AUDIO_ERR_MEM_LACK) {
                    ESP_LOGE(TAG,
                             "decode MEM_LACK ret=%d heap=%u largest=%u pcm_chunks=%u bad=%u",
                             (int)ret, (unsigned)InternalHeapFree(),
                             (unsigned)InternalHeapLargest(), (unsigned)ctx->pcm_chunks,
                             (unsigned)ctx->frames_bad);
                    if (ctx->pcm_chunks == 0) {
                        ctx->format_rejected = true;
                        if (ctx->page != nullptr) {
                            ctx->page->SetStatusHint("mp3 mem lack");
                        }
                    }
                    break;
                }
                if ((ctx->frames_bad % 200) == 1) {
                    ESP_LOGW(TAG, "decode ret=%d (%s) (bad=%u ok=%u heap=%u largest=%u)",
                             (int)ret, AudioErrName(ret), (unsigned)ctx->frames_bad,
                             (unsigned)ctx->frames_ok, (unsigned)InternalHeapFree(),
                             (unsigned)InternalHeapLargest());
                }
                // Only bail out if the decoder has never produced a single sample.
                // A stream start carries ID3 tags and a partial frame, so early
                // errors are normal and must not kill an otherwise healthy stream.
                if (++ctx->consec_bad > 2000 && ctx->pcm_chunks == 0) {
                    ESP_LOGE(TAG, "decoder failed %u times with no PCM ever; aborting",
                             (unsigned)ctx->consec_bad);
                    ctx->format_rejected = true;
                    if (ctx->page != nullptr) {
                        ctx->page->SetStatusHint("decode failed");
                    }
                }
                break;
            }
            ctx->consec_bad = 0;
            ++ctx->frames_ok;
            // ESP_AUDIO_ERR_OK also covers "input cached, nothing decoded yet", so
            // decoded_size is the only thing that says PCM actually exists.
            if (frame.decoded_size > 0) {
                if (frame.decoded_size > ctx->pcm.size()) {
                    ESP_LOGE(TAG, "decoder overran sink: %u > %u", (unsigned)frame.decoded_size,
                             (unsigned)ctx->pcm.size());
                    ctx->format_rejected = true;
                    break;
                }
                FeedPcmToSpeaker(ctx, frame.buffer, frame.decoded_size);
            }
            break;
        }

        if (ctx->format_rejected) {
            break;
        }
        if (raw.consumed == 0) {
            if (had_error) {
                // Bad byte the parser could not use: slide one byte so we always
                // make progress instead of retrying the same data forever.
                ++raw.buffer;
                --raw.len;
                ++off;
                continue;
            }
            // Parser wants more data before it can complete a frame; keep the
            // remainder and come back after the next socket read.
            break;
        }
        const uint32_t step = raw.consumed > raw.len ? raw.len : raw.consumed;
        raw.buffer += step;
        raw.len -= step;
        off += static_cast<int>(step);
        raw.consumed = 0;
    }
    return off;
}

bool OpenDecoder(StreamCtx* ctx) {
    if (ctx->dec != nullptr) {
        esp_audio_simple_dec_close(ctx->dec);
        ctx->dec = nullptr;
    }
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    // Keep the library parser. It handles ID3 tags, initial sync and free-format
    // frames that a hand-rolled frame splitter gets wrong, and now that the decoder
    // is allocated first there is room for it.
    cfg.use_frame_dec = false;
    cfg.dec_cfg = nullptr;
    cfg.cfg_size = 0;
    const size_t before = InternalHeapFree();
    const size_t before_largest = InternalHeapLargest();
    const esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &ctx->dec);
    const size_t after = InternalHeapFree();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "mp3 decoder open failed ret=%d (%s) heap free=%u largest=%u", (int)ret,
                 AudioErrName(ret), (unsigned)before, (unsigned)before_largest);
        ctx->dec = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "mp3 decoder open ok: used %uB, heap %u -> %u (largest %u -> %u)",
             (unsigned)(before - after), (unsigned)before, (unsigned)after,
             (unsigned)before_largest, (unsigned)InternalHeapLargest());
    ctx->sample_rate = 0;
    ctx->channels = 0;
    ctx->resample_pos = 0.0;
    return true;
}

// Speaker self-test. If this beep is audible the ES8311 route, PA, volume and I2S
// clocking are all fine and any remaining silence is a network/decode problem.
void PlayTestTone(int ms, int hz) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    const int rate = codec->output_sample_rate();
    if (rate <= 0) {
        return;
    }
    const int block = rate / 50;  // 20 ms
    const int blocks = (ms * rate / 1000) / block;
    std::vector<int16_t> buf(block);
    double phase = 0.0;
    const double step = 2.0 * M_PI * hz / rate;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            buf[i] = static_cast<int16_t>(8000.0 * std::sin(phase));
            phase += step;
        }
        codec->OutputData(buf);
        Application::GetInstance().GetAudioService().NotifyOutputActivity();
    }
    ESP_LOGI(TAG, "self-test tone done: %dHz %dms at %dHz vol=%d out_en=%d", hz, ms, rate,
             codec->output_volume(), codec->output_enabled() ? 1 : 0);
}

bool PlayStationOnce(StreamCtx* ctx, const Station& station, uint32_t bound_gen) {
    RadioPage* page = ctx->page;
    if (page == nullptr || !page->IsStreamRunning()) {
        return false;
    }
    const size_t heap = InternalHeapFree();
    const size_t largest = InternalHeapLargest();
    if (heap < kMinHeapToStream) {
        // Below this, getaddrinfo() fails with EAI_MEMORY and esp_http_client's
        // allocations start returning NULL, which is how this page used to die.
        ESP_LOGE(TAG, "refusing to stream: only %uB free (largest=%u)", (unsigned)heap,
                 (unsigned)largest);
        page->SetStatusHint("low memory");
        vTaskDelay(pdMS_TO_TICKS(1000));
        return false;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = station.url;
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = kHttpChunk;
    cfg.buffer_size_tx = 512;
    cfg.user_agent = "Mozilla/5.0 (xiaozhi-ADV-car/radio)";
    cfg.max_redirection_count = 5;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        page->SetStatusHint("http init fail");
        return false;
    }
    page->PublishActiveHttp(client);

    ESP_LOGI(TAG, "open %s heap=%u largest=%u stack_free=%u", station.url, (unsigned)heap,
             (unsigned)largest, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    page->SetStatusHint("connecting");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open fail %s", esp_err_to_name(err));
        page->SetStatusHint("connect fail");
        page->ClearActiveHttp(client);
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d", status);
        page->SetStatusHint("http error");
        page->ClearActiveHttp(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // The decoder is opened once for the whole task (see StreamTask); reset it so a
    // reconnect starts from a clean state without paying the allocation again.
    esp_audio_simple_dec_reset(ctx->dec);
    ctx->sample_rate = 0;
    ctx->channels = 0;
    ctx->resample_pos = 0.0;
    ctx->consec_bad = 0;

    std::vector<uint8_t> inbuf(kInBufBytes);
    int fill = 0;
    uint32_t total = 0;
    const uint32_t pcm_at_start = ctx->pcm_chunks;
    int idle_reads = 0;
    uint32_t reads = 0;
    // Every loop exit sets this, so the serial log always names the precise reason
    // the stream stopped instead of just going quiet.
    const char* exit_reason = "user stop / page change";

    while (page->IsStreamRunning() && page->StationGeneration() == bound_gen) {
        if (page->IsUserPaused()) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        int room = kInBufBytes - fill;
        if (room < kHttpChunk) {
            // No frame header found in a whole buffer: drop the oldest half.
            const int drop = kInBufBytes / 2;
            memmove(inbuf.data(), inbuf.data() + drop, kInBufBytes - drop);
            fill -= drop;
            room = kInBufBytes - fill;
        }
        const int n = esp_http_client_read(client, reinterpret_cast<char*>(inbuf.data() + fill),
                                           room < kHttpChunk ? room : kHttpChunk);
        if (n < 0) {
            ESP_LOGW(TAG, "read err %d after %uB pcm=%u", n, (unsigned)total,
                     (unsigned)(ctx->pcm_chunks - pcm_at_start));
            exit_reason = "http read error";
            break;
        }
        if (n == 0) {
            // A live stream can legitimately stall for a moment. Only give up after
            // ~1s of nothing, so a hiccup is not mistaken for end-of-stream.
            if (++idle_reads > 20) {
                ESP_LOGW(TAG, "no data for ~1s after %uB", (unsigned)total);
                exit_reason = "stream idle (EOF or timeout)";
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        idle_reads = 0;
        fill += n;
        total += static_cast<uint32_t>(n);
        ctx->bytes_in += static_cast<uint32_t>(n);

        const uint32_t pcm_before = ctx->pcm_chunks;
        const int used = DecodeBuffered(ctx, inbuf.data(), fill);
        // Trace the first reads in full, then thin out: this is what shows whether
        // the stream dies at the socket, at the decoder, or at the codec write.
        if (++reads <= 8 || (reads % 100) == 0) {
            ESP_LOGI(TAG, "read#%u n=%d fill=%d used=%d ok=%u bad=%u writes+=%u total=%uB",
                     (unsigned)reads, n, fill, used, (unsigned)ctx->frames_ok,
                     (unsigned)ctx->frames_bad, (unsigned)(ctx->pcm_chunks - pcm_before),
                     (unsigned)total);
        }
        if (used > 0 && used < fill) {
            memmove(inbuf.data(), inbuf.data() + used, fill - used);
        }
        fill -= used;

        if (ctx->format_rejected) {
            ESP_LOGE(TAG, "stopping: decoder unusable for this stream");
            exit_reason = "decoder rejected/failed";
            break;
        }
        if (ctx->pcm_chunks == pcm_at_start && total >= kNoPcmAbortBytes) {
            ESP_LOGW(TAG, "abort: %uB in, no PCM (ok=%u bad=%u)", (unsigned)total,
                     (unsigned)ctx->frames_ok, (unsigned)ctx->frames_bad);
            exit_reason = "no PCM produced";
            break;
        }
        // vTaskDelay, not taskYIELD: taskYIELD never lets the lower-priority IDLE
        // task run, which is what starves the CPU1 idle watchdog.
        vTaskDelay(1);
    }

    page->ClearActiveHttp(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const uint32_t pcm_delta = ctx->pcm_chunks - pcm_at_start;
    ESP_LOGI(TAG, "session end [%s] bytes=%u reads=%u pcm_writes=%u frames_ok=%u bad=%u heap=%u stack_free=%u",
             exit_reason, (unsigned)total, (unsigned)reads, (unsigned)pcm_delta,
             (unsigned)ctx->frames_ok, (unsigned)ctx->frames_bad,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (pcm_delta == 0) {
        page->SetStatusHint("no audio data");
        return false;
    }
    return true;
}

}  // namespace

void RadioPage::NotifyLevel(float level) {
    if (level < 0.0f) {
        level = 0.0f;
    }
    if (level > 1.0f) {
        level = 1.0f;
    }
    level_.store(level);
}

void RadioPage::NotifyBandLevels(const float* energies, int count) {
    if (energies == nullptr || count != kBarCount) {
        return;
    }

    float max_e = 0.001f;
    for (int b = 0; b < kBarCount; ++b) {
        if (energies[b] > max_e) {
            max_e = energies[b];
        }
    }
    if (max_e > band_peak_) {
        band_peak_ = max_e;
    } else {
        band_peak_ = band_peak_ * 0.92f + max_e * 0.08f;
    }
    if (band_peak_ < 80.0f) {
        band_peak_ = 80.0f;
    }

    for (int b = 0; b < kBarCount; ++b) {
        float target = energies[b] / band_peak_;
        if (target > 1.0f) {
            target = 1.0f;
        }
        if (target > band_levels_[b]) {
            band_levels_[b] = band_levels_[b] * 0.35f + target * 0.65f;
        } else {
            band_levels_[b] = band_levels_[b] * 0.75f + target * 0.25f;
        }
    }
}

void RadioPage::NotifyPlaying() {
    if (!user_paused_.load()) {
        play_state_.store(RadioPlayState::Playing);
    }
}

void RadioPage::SetStatusHint(const char* hint) {
    if (hint == nullptr) {
        status_hint_[0] = '\0';
        return;
    }
    std::snprintf(status_hint_, sizeof(status_hint_), "%s", hint);
}

void RadioPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ != nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    StripStyles(panel_);
    lv_obj_set_size(panel_, kScreenW, kScreenH);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);

    play_label_ = lv_label_create(panel_);
    lv_label_set_text(play_label_, LV_SYMBOL_PLAY " LIVE");
    lv_obj_set_style_text_font(play_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(play_label_, lv_color_hex(0x00FF66), 0);
    lv_obj_set_pos(play_label_, 6, 4);

    listening_label_ = lv_label_create(panel_);
    lv_label_set_text(listening_label_, "connecting");
    lv_obj_set_style_text_font(listening_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(listening_label_, lv_color_hex(0x00FF66), 0);
    lv_obj_align(listening_label_, LV_ALIGN_TOP_MID, 10, 4);

    vol_label_ = lv_label_create(panel_);
    lv_label_set_text(vol_label_, "vol85");
    lv_obj_set_style_text_font(vol_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_label_, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(vol_label_, LV_ALIGN_TOP_RIGHT, -6, 4);

    station_label_ = lv_label_create(panel_);
    lv_label_set_text(station_label_, kStations[kDefaultStation].title);
    lv_obj_set_style_text_font(station_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(station_label_, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_pos(station_label_, 6, 24);

    status_label_ = lv_label_create(panel_);
    lv_label_set_text(status_label_, "1/N News  2/M Music");
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(status_label_, 6, 44);

    const int plot_w = kBarCount * (kBarW + kBarGap) - kBarGap;
    const int plot_x = (kScreenW - plot_w) / 2;
    for (int i = 0; i < kBarCount; ++i) {
        const int x = plot_x + i * (kBarW + kBarGap);
        lv_obj_t* grid = lv_obj_create(panel_);
        StripStyles(grid);
        lv_obj_set_size(grid, 1, kPlotH);
        lv_obj_set_pos(grid, x + kBarW / 2, kPlotTop);
        lv_obj_set_style_bg_color(grid, lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);

        bars_[i] = lv_obj_create(panel_);
        StripStyles(bars_[i]);
        lv_obj_set_size(bars_[i], kBarW, 2);
        lv_obj_set_pos(bars_[i], x, kPlotTop + kPlotH - 2);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(0x00FFFF), 0);
        lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
        band_levels_[i] = 0;
    }

    lv_obj_t* hint = lv_label_create(panel_);
    lv_label_set_text(hint, "P pause  ;/. vol");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

void RadioPage::PublishActiveHttp(void* client) {
    active_http_.store(client);
}

void RadioPage::ClearActiveHttp(void* expect) {
    void* expected = expect;
    active_http_.compare_exchange_strong(expected, nullptr);
}

void RadioPage::AbortActiveHttp() {
    void* client = active_http_.exchange(nullptr);
    if (client == nullptr) {
        return;
    }
    // Close only: the stream task still owns cleanup. Closing the socket unblocks
    // esp_http_client_open/read so StopStream can join without vTaskDelete.
    ESP_LOGW(TAG, "AbortActiveHttp: closing socket to unblock stream task");
    esp_http_client_close(static_cast<esp_http_client_handle_t>(client));
}

bool RadioPage::WaitStreamExit(TickType_t ticks, const char* why) {
    if (stream_done_ == nullptr) {
        return !stream_alive_.load() && stream_task_ == nullptr;
    }
    if (!stream_alive_.load() && stream_task_ == nullptr) {
        // Consume a stale done token left by a prior timed-out join.
        xSemaphoreTake(stream_done_, 0);
        return true;
    }
    ESP_LOGI(TAG, "WaitStreamExit (%s) alive=%d task=%p", why, stream_alive_.load() ? 1 : 0,
             stream_task_);
    if (xSemaphoreTake(stream_done_, ticks) == pdTRUE) {
        stream_alive_.store(false);
        return true;
    }
    ESP_LOGE(TAG, "WaitStreamExit (%s) timeout alive=%d task=%p heap=%u largest=%u", why,
             stream_alive_.load() ? 1 : 0, stream_task_, (unsigned)InternalHeapFree(),
             (unsigned)InternalHeapLargest());
    return false;
}

void RadioPage::CaptureAudioExclusive() {
    auto& audio = Application::GetInstance().GetAudioService();
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(false);
    // Always re-run Release: Opus may already be freed after a prior Radio visit
    // (we no longer restore on exclusive leave), or Chat may have rebuilt them.
    // ReleaseAudioModels is idempotent when null.
    const size_t before = InternalHeapFree();
    const size_t before_largest = InternalHeapLargest();
    audio.ReleaseAudioModels();
    const size_t after = InternalHeapFree();
    const size_t after_largest = InternalHeapLargest();
    // Hold TX on; force RX off so a leftover Music mic path cannot stall duplex I2S.
    audio.SetExternalPlaybackActive(true);
    if (audio_exclusive_) {
        ESP_LOGW(TAG, "audio exclusive re-assert heap %u->%u largest %u->%u", (unsigned)before,
                 (unsigned)after, (unsigned)before_largest, (unsigned)after_largest);
    } else {
        audio_exclusive_ = true;
        ESP_LOGI(TAG, "audio exclusive ON heap %u->%u largest %u->%u", (unsigned)before,
                 (unsigned)after, (unsigned)before_largest, (unsigned)after_largest);
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableInput(false);
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        codec->SetOutputVolume(kDefaultVolume);
        ESP_LOGI(TAG, "codec vol=%d out=%d in=%d rate=%d heap=%u largest=%u", codec->output_volume(),
                 codec->output_enabled() ? 1 : 0, codec->input_enabled() ? 1 : 0,
                 codec->output_sample_rate(), (unsigned)InternalHeapFree(),
                 (unsigned)InternalHeapLargest());
    }
}

void RadioPage::ReleaseAudioExclusive() {
    // Soft release only: clear the TX hold and drop the exclusive flag. Do NOT
    // RestoreAudioModels / RestoreAudioRouting here — Radio↔Car/Music would
    // rebuild Opus (~43KB) just to free it again on the next Radio enter, and
    // that thrash fragments the no-PSRAM heap so MP3 open/decode fails silently.
    // Chat OnEnter → RestoreAudioRouting() rebuilds Opus when TTS is needed.
    if (!audio_exclusive_) {
        Application::GetInstance().GetAudioService().SetExternalPlaybackActive(false);
        ESP_LOGI(TAG, "audio exclusive OFF (already clear) heap=%u",
                 (unsigned)InternalHeapFree());
        return;
    }
    audio_exclusive_ = false;
    auto& audio = Application::GetInstance().GetAudioService();
    audio.SetExternalPlaybackActive(false);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableInput(false);
    }
    ESP_LOGI(TAG, "audio exclusive OFF (models deferred) heap=%u largest=%u",
             (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest());
}

void RadioPage::StreamTask(void* arg) {
    auto* self = static_cast<RadioPage*>(arg);

    self->stream_alive_.store(true);
    ESP_LOGI(TAG, "StreamTask start heap=%u largest=%u stack=%d",
             (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest(), kTaskStack);

    auto finish = [self](const char* why) {
        if (self->active_http_.load() != nullptr) {
            self->active_http_.store(nullptr);
        }
        self->stream_alive_.store(false);
        self->play_state_.store(RadioPlayState::Idle);
        ESP_LOGI(TAG, "StreamTask finish (%s) heap=%u", why, (unsigned)InternalHeapFree());
        xSemaphoreGive(self->stream_done_);
        vTaskDelete(nullptr);
    };

    static bool mp3_registered = false;
    if (!mp3_registered) {
        const esp_audio_err_t reg = esp_mp3_dec_register();
        const esp_audio_err_t sup =
            esp_audio_simple_check_audio_type(ESP_AUDIO_SIMPLE_DEC_TYPE_MP3);
        if (reg != ESP_AUDIO_ERR_OK || sup != ESP_AUDIO_ERR_OK) {
            // Bail out loudly. Playing on without a decoder is what sends raw MP3
            // bytes to the speaker as if they were PCM.
            ESP_LOGE(TAG, "MP3 unavailable (register=%d check=%d); CONFIG_AUDIO_DECODER_MP3_SUPPORT?",
                     (int)reg, (int)sup);
            self->play_state_.store(RadioPlayState::Error);
            self->SetStatusHint("no mp3 decoder");
            finish("no mp3");
            return;
        }
        mp3_registered = true;
        ESP_LOGI(TAG, "MP3 decoder registered and supported");
    }

    StreamCtx ctx;
    ctx.page = self;

    // Open the decoder before anything else claims memory: it is by far the largest
    // single allocation, and with ~44KB of internal SRAM on this board it only fits
    // if it gets first pick. It then stays open for the life of the task.
    if (!OpenDecoder(&ctx)) {
        self->play_state_.store(RadioPlayState::Error);
        self->SetStatusHint("no memory for mp3");
        finish("decoder open fail");
        return;
    }

    ctx.pcm.resize(kPcmOutInit);
    ctx.resample.resize(kResampleChunk);
    ESP_LOGI(TAG, "buffers ready, heap=%u largest=%u", (unsigned)InternalHeapFree(),
             (unsigned)InternalHeapLargest());

    PlayTestTone(kToneMs, kToneHz);

    int fail_streak = 0;
    uint32_t bound_gen = self->StationGeneration();

    while (self->stream_run_.load()) {
        bound_gen = self->StationGeneration();
        int idx = self->StationIndex();
        if (idx < 0 || idx >= kStationCount) {
            idx = kDefaultStation;
        }

        if (self->IsUserPaused()) {
            self->play_state_.store(RadioPlayState::Paused);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        self->play_state_.store(RadioPlayState::Connecting);
        const bool ok = PlayStationOnce(&ctx, kStations[idx], bound_gen);

        if (!self->stream_run_.load()) {
            break;
        }
        if (ok) {
            fail_streak = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ++fail_streak;
        ESP_LOGW(TAG, "play fail streak=%d station=%s", fail_streak, kStations[idx].short_name);
        if (fail_streak >= 3) {
            self->play_state_.store(RadioPlayState::Error);
            if (self->status_hint_[0] == '\0') {
                self->SetStatusHint("stream fail");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    if (ctx.dec != nullptr) {
        esp_audio_simple_dec_close(ctx.dec);
        ctx.dec = nullptr;
    }

    ESP_LOGI(TAG, "StreamTask exit pcm=%u bytes=%u stack_free=%u", (unsigned)ctx.pcm_chunks,
             (unsigned)ctx.bytes_in, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    finish("normal exit");
}

void RadioPage::StartStream() {
    if (!active_) {
        ESP_LOGW(TAG, "StartStream skipped: page inactive heap=%u",
                 (unsigned)InternalHeapFree());
        return;
    }
    if (stream_done_ == nullptr) {
        stream_done_ = xSemaphoreCreateBinary();
        if (stream_done_ == nullptr) {
            ESP_LOGE(TAG, "semaphore alloc failed");
            play_state_.store(RadioPlayState::Error);
            SetStatusHint("out of memory");
            return;
        }
    }

    // A prior StopStream that timed out must be drained before we touch Opus/MP3
    // again — otherwise two tasks share the codec and the heap is double-booked.
    if (stream_alive_.load() || stream_task_ != nullptr) {
        ESP_LOGW(TAG, "StartStream: prior task still alive=%d task=%p — draining",
                 stream_alive_.load() ? 1 : 0, stream_task_);
        stream_run_.store(false);
        AbortActiveHttp();
        if (!WaitStreamExit(pdMS_TO_TICKS(kStreamJoinMs), "before restart")) {
            ESP_LOGE(TAG, "StartStream refuse: prior stream stuck heap=%u largest=%u",
                     (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest());
            play_state_.store(RadioPlayState::Error);
            SetStatusHint("stream busy");
            return;
        }
        stream_task_ = nullptr;
    } else {
        // Drop a token left behind by a join that timed out, so the next StopStream
        // cannot return while this new task is still alive.
        xSemaphoreTake(stream_done_, 0);
    }

    // Take the codec here, on the main loop, before allocating the stack: tearing
    // down the wake-word/AFE pipeline releases a large block of internal SRAM, and
    // this board does not have enough free to carve out the task stack without it.
    // Capture/release now pair on the same thread (StartStream / StopStream).
    ESP_LOGI(TAG, "heap before capture free=%u largest=%u exclusive=%d",
             (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest(),
             audio_exclusive_ ? 1 : 0);
    CaptureAudioExclusive();
    ESP_LOGI(TAG, "heap after capture free=%u largest=%u", (unsigned)InternalHeapFree(),
             (unsigned)InternalHeapLargest());

    if (InternalHeapFree() < kMinHeapToStream) {
        ESP_LOGE(TAG, "StartStream refuse: heap %u < %u after ReleaseAudioModels",
                 (unsigned)InternalHeapFree(), (unsigned)kMinHeapToStream);
        play_state_.store(RadioPlayState::Error);
        SetStatusHint("low memory");
        ReleaseAudioExclusive();
        return;
    }

    stream_run_.store(true);
    user_paused_.store(false);
    play_state_.store(RadioPlayState::Connecting);
    level_.store(0.0f);
    band_peak_ = 200.0f;
    for (int i = 0; i < kBarCount; ++i) {
        band_levels_[i] = 0.0f;
    }
    SetStatusHint("connecting");

    const BaseType_t ok = xTaskCreatePinnedToCore(StreamTask, "radio_stream", kTaskStack, this,
                                                  kTaskPrio, &stream_task_, kTaskCore);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream task create failed heap=%u largest=%u",
                 (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest());
        stream_run_.store(false);
        stream_task_ = nullptr;
        stream_alive_.store(false);
        play_state_.store(RadioPlayState::Error);
        SetStatusHint("low memory");
        ReleaseAudioExclusive();
    } else {
        ESP_LOGI(TAG, "StartStream ok task=%p", stream_task_);
    }
}

void RadioPage::StopStream() {
    stream_run_.store(false);
    user_paused_.store(false);
    AbortActiveHttp();

    if (!stream_alive_.load() && stream_task_ == nullptr) {
        ReleaseAudioExclusive();
        return;
    }

    ESP_LOGI(TAG, "StopStream begin alive=%d task=%p heap=%u", stream_alive_.load() ? 1 : 0,
             stream_task_, (unsigned)InternalHeapFree());

    // Bounded join. AbortActiveHttp should unblock open/read within ms; the long
    // ceiling only covers the rare case where close did not wake the caller.
    if (!WaitStreamExit(pdMS_TO_TICKS(kStreamJoinMs), "StopStream")) {
        // Do NOT null stream_task_ and do NOT Restore Opus: the zombie still holds
        // the MP3 decoder (~25KB). Restoring Opus on top is what made re-enter fail
        // with MEM_LACK / silent "connecting". StartStream will drain it next time.
        ESP_LOGE(TAG, "StopStream: join timeout — keeping exclusive (no Opus restore yet)");
        play_state_.store(RadioPlayState::Idle);
        Application::GetInstance().Schedule([this]() {
            if (stream_alive_.load() || stream_task_ != nullptr) {
                AbortActiveHttp();
                if (!WaitStreamExit(pdMS_TO_TICKS(kStreamJoinMs), "deferred leave")) {
                    ESP_LOGE(TAG, "deferred leave still stuck");
                    return;
                }
            }
            stream_task_ = nullptr;
            if (!active_) {
                ReleaseAudioExclusive();
            }
        });
        return;
    }
    stream_task_ = nullptr;
    play_state_.store(RadioPlayState::Idle);
    level_.store(0.0f);
    band_peak_ = 200.0f;
    for (int i = 0; i < kBarCount; ++i) {
        band_levels_[i] = 0.0f;
    }
    ReleaseAudioExclusive();
    ESP_LOGI(TAG, "StopStream end heap=%u largest=%u", (unsigned)InternalHeapFree(),
             (unsigned)InternalHeapLargest());
}

void RadioPage::SelectStation(int index) {
    if (index < 0 || index >= kStationCount || station_index_.load() == index) {
        return;
    }
    station_index_.store(index);
    station_gen_.fetch_add(1);
    user_paused_.store(false);
    play_state_.store(RadioPlayState::Connecting);
    SetStatusHint("switching");
    ESP_LOGI(TAG, "SelectStation %d %s", index, kStations[index].short_name);
}

void RadioPage::TogglePause() {
    const bool next = !user_paused_.load();
    user_paused_.store(next);
    play_state_.store(next ? RadioPlayState::Paused : RadioPlayState::Connecting);
}

void RadioPage::AdjustVolume(int delta) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int vol = codec->output_volume() + delta;
    if (vol < 10) {
        vol = 10;
    }
    if (vol > 100) {
        vol = 100;
    }
    codec->SetOutputVolume(vol);
}

void RadioPage::UpdateUi(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr) {
        return;
    }

    const RadioPlayState st = play_state_.load();
    int st_idx = station_index_.load();
    if (st_idx < 0 || st_idx >= kStationCount) {
        st_idx = 0;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    const int vol = codec != nullptr ? codec->output_volume() : 0;

    DisplayLockGuard lock(display);
    if (station_label_ != nullptr) {
        lv_label_set_text(station_label_, kStations[st_idx].title);
    }
    if (vol_label_ != nullptr) {
        char buf[16];
        snprintf(buf, sizeof(buf), "vol%d", vol);
        lv_label_set_text(vol_label_, buf);
    }
    if (listening_label_ != nullptr) {
        const char* text = "idle";
        uint32_t color = 0x888888;
        switch (st) {
            case RadioPlayState::Connecting:
                text = "connecting";
                color = 0xFFAA00;
                break;
            case RadioPlayState::Playing:
                text = "listening";
                color = 0x00FF66;
                break;
            case RadioPlayState::Paused:
                text = "paused";
                color = 0xFFAA00;
                break;
            case RadioPlayState::Error:
                text = "stream err";
                color = 0xFF5555;
                break;
            default:
                break;
        }
        lv_label_set_text(listening_label_, text);
        lv_obj_set_style_text_color(listening_label_, lv_color_hex(color), 0);
    }
    if (play_label_ != nullptr) {
        if (st == RadioPlayState::Playing) {
            lv_label_set_text(play_label_, LV_SYMBOL_PLAY " LIVE");
            lv_obj_set_style_text_color(play_label_, lv_color_hex(0x00FF66), 0);
        } else if (st == RadioPlayState::Paused) {
            lv_label_set_text(play_label_, LV_SYMBOL_PAUSE " HOLD");
            lv_obj_set_style_text_color(play_label_, lv_color_hex(0xFFAA00), 0);
        } else if (st == RadioPlayState::Connecting) {
            lv_label_set_text(play_label_, LV_SYMBOL_REFRESH " LIVE");
            lv_obj_set_style_text_color(play_label_, lv_color_hex(0xFFAA00), 0);
        } else {
            lv_label_set_text(play_label_, LV_SYMBOL_STOP " LIVE");
            lv_obj_set_style_text_color(play_label_, lv_color_hex(0x666666), 0);
        }
    }
    if (status_label_ != nullptr) {
        if ((st == RadioPlayState::Error || st == RadioPlayState::Connecting) &&
            status_hint_[0] != '\0') {
            lv_label_set_text(status_label_, status_hint_);
            lv_obj_set_style_text_color(
                status_label_, lv_color_hex(st == RadioPlayState::Error ? 0xFF5555 : 0xAAAAAA), 0);
        } else {
            lv_label_set_text(status_label_, st_idx == 0 ? "[News] Music  ,/ / N/M"
                                                        : "News [Music]  ,/ / N/M");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0xAAAAAA), 0);
        }
    }

    if (st != RadioPlayState::Playing) {
        for (int b = 0; b < kBarCount; ++b) {
            band_levels_[b] *= 0.85f;
        }
    }

    const int plot_w = kBarCount * (kBarW + kBarGap) - kBarGap;
    const int plot_x = (kScreenW - plot_w) / 2;
    for (int i = 0; i < kBarCount; ++i) {
        if (bars_[i] == nullptr) {
            continue;
        }
        int h = static_cast<int>(band_levels_[i] * (kPlotH - 2));
        if (h < 2) {
            h = 2;
        }
        if (h > kPlotH) {
            h = kPlotH;
        }
        const int x = plot_x + i * (kBarW + kBarGap);
        lv_obj_set_size(bars_[i], kBarW, h);
        lv_obj_set_pos(bars_[i], x, kPlotTop + kPlotH - h);

        uint32_t color = (i == 19) ? 0x22C55E : 0x00FFFF;
        if (band_levels_[i] > 0.85f && (i % 11 == 3)) {
            color = 0x22C55E;
        }
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(color), 0);
    }
}

void RadioPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    const uint32_t gen = enter_gen_.fetch_add(1) + 1;
    band_peak_ = 200.0f;
    for (int i = 0; i < kBarCount; ++i) {
        band_levels_[i] = 0.0f;
    }
    if (station_index_.load() < 0 || station_index_.load() >= kStationCount) {
        station_index_.store(kDefaultStation);
    }
    ESP_LOGI(TAG, "OnEnter gen=%u station=%d (%s) heap=%u largest=%u exclusive=%d alive=%d",
             (unsigned)gen, station_index_.load(), kStations[station_index_.load()].short_name,
             (unsigned)InternalHeapFree(), (unsigned)InternalHeapLargest(),
             audio_exclusive_ ? 1 : 0, stream_alive_.load() ? 1 : 0);

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        active_ = false;
        return;
    }
    {
        DisplayLockGuard lock(display);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
        if (station_label_ != nullptr) {
            lv_label_set_text(station_label_, kStations[station_index_.load()].title);
        }
    }
    display->HideChatUi();
    user_paused_.store(false);
    play_state_.store(RadioPlayState::Connecting);
    SetStatusHint("connecting");
    // Deferred so PageManager::ShowPage can clear switching_ first. Capturing `this`
    // is safe: RadioPage is a by-value member of PageManager and outlives the app.
    // enter_gen_ invalidates a StartStream that was queued before a leave/re-enter.
    Application::GetInstance().Schedule([this, gen]() {
        if (!active_ || enter_gen_.load() != gen) {
            ESP_LOGW(TAG, "StartStream schedule dropped gen=%u now=%u active=%d", (unsigned)gen,
                     (unsigned)enter_gen_.load(), active_ ? 1 : 0);
            return;
        }
        StartStream();
    });
}

void RadioPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    // Invalidate any pending StartStream before stopping, so a late Schedule cannot
    // recreate the stream after we have released (or while we are releasing).
    enter_gen_.fetch_add(1);
    active_ = false;
    ESP_LOGI(TAG, "OnLeave gen=%u alive=%d exclusive=%d heap=%u", (unsigned)enter_gen_.load(),
             stream_alive_.load() ? 1 : 0, audio_exclusive_ ? 1 : 0,
             (unsigned)InternalHeapFree());
    StopStream();
    if (display == nullptr || panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void RadioPage::Tick(CardputerAdvCarLcdDisplay* display) {
    UpdateUi(display);
}

bool RadioPage::HandleKey(const KeyEvent& event) {
    if (!active_ || !event.pressed || event.is_modifier) {
        return true;
    }
    const char ch = event.key_char ? static_cast<char>(std::tolower(
                                         static_cast<unsigned char>(event.key_char[0])))
                                   : '\0';

    if (event.key_code == KC_1 || event.key_code == KC_N || event.key_code == KC_COMMA ||
        ch == '1' || ch == 'n' || ch == ',') {
        SelectStation(0);
        return true;
    }
    if (event.key_code == KC_2 || event.key_code == KC_M || event.key_code == KC_SLASH ||
        ch == '2' || ch == 'm' || ch == '/') {
        SelectStation(1);
        return true;
    }
    if (event.key_code == KC_P || event.key_code == KC_SPACE || ch == 'p' || ch == ' ') {
        TogglePause();
        return true;
    }
    if (event.key_code == KC_SEMICOLON || event.key_code == KC_UP) {
        AdjustVolume(5);
        return true;
    }
    if (event.key_code == KC_DOT || event.key_code == KC_DOWN) {
        AdjustVolume(-5);
        return true;
    }
    return true;
}
