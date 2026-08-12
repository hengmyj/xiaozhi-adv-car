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
constexpr lv_coord_t kBarW = 8;
constexpr lv_coord_t kBarGap = 3;
constexpr lv_coord_t kPlotTop = 62;
constexpr lv_coord_t kPlotH = 48;

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
constexpr int kDefaultVolume = 85;
constexpr uint32_t kNoPcmAbortBytes = 48 * 1024;

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
    uint32_t pcm_chunks = 0;
    uint32_t bytes_in = 0;
    uint32_t frames_ok = 0;
    uint32_t frames_bad = 0;
    uint32_t info_misses = 0;
    uint32_t consec_bad = 0;
    bool format_rejected = false;
    double resample_pos = 0.0;
};

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
        out[n_out++] = static_cast<int16_t>(sample);
        energy += static_cast<double>(sample) * static_cast<double>(sample);
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
    if (energy_n > 0) {
        const float rms = static_cast<float>(std::sqrt(energy / energy_n) / 32768.0);
        ctx->ema_level = ctx->ema_level * 0.7f + rms * 0.3f;
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
                if ((ctx->frames_bad % 200) == 1) {
                    ESP_LOGW(TAG, "decode ret=%d (bad=%u ok=%u)", (int)ret,
                             (unsigned)ctx->frames_bad, (unsigned)ctx->frames_ok);
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
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &ctx->dec);
    const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "mp3 decoder open failed ret=%d (heap was %uB)", (int)ret,
                 (unsigned)before);
        ctx->dec = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "mp3 decoder open ok: used %uB, heap %u -> %u", (unsigned)(before - after),
             (unsigned)before, (unsigned)after);
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
    const size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (heap < kMinHeapToStream) {
        // Below this, getaddrinfo() fails with EAI_MEMORY and esp_http_client's
        // allocations start returning NULL, which is how this page used to die.
        ESP_LOGE(TAG, "refusing to stream: only %uB internal heap free", (unsigned)heap);
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

    ESP_LOGI(TAG, "open %s heap=%u stack_free=%u", station.url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    page->SetStatusHint("connecting");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open fail %s", esp_err_to_name(err));
        page->SetStatusHint("connect fail");
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d", status);
        page->SetStatusHint("http error");
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
        bars_[i] = lv_obj_create(panel_);
        StripStyles(bars_[i]);
        lv_obj_set_size(bars_[i], kBarW, 2);
        lv_obj_set_pos(bars_[i], plot_x + i * (kBarW + kBarGap), kPlotTop + kPlotH - 2);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(0x00FF66), 0);
        lv_obj_set_style_bg_opa(bars_[i], LV_OPA_COVER, 0);
    }

    lv_obj_t* hint = lv_label_create(panel_);
    lv_label_set_text(hint, "P pause  ;/. vol");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

void RadioPage::CaptureAudioExclusive() {
    if (audio_exclusive_) {
        return;
    }
    auto& audio = Application::GetInstance().GetAudioService();
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(false);
    // Stopping them is not enough. The MP3 decoder core needs ~25KB and this board
    // has no PSRAM, so the Opus pair (unused while the radio plays) must actually be
    // freed or esp_mp3_dec fails to init with ESP_AUDIO_ERR_MEM_LACK.
    audio.ReleaseAudioModels();
    // This is the only thing that should touch codec power: it enables output and
    // pins it on for as long as the hold is active.
    audio.SetExternalPlaybackActive(true);
    audio_exclusive_ = true;

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(kDefaultVolume);
        ESP_LOGI(TAG, "audio exclusive ON vol=%d out=%d in=%d rate=%d", codec->output_volume(),
                 codec->output_enabled() ? 1 : 0, codec->input_enabled() ? 1 : 0,
                 codec->output_sample_rate());
    }
}

void RadioPage::ReleaseAudioExclusive() {
    if (!audio_exclusive_) {
        return;
    }
    audio_exclusive_ = false;
    auto& app = Application::GetInstance();
    app.GetAudioService().SetExternalPlaybackActive(false);
    // Rebuild the Opus codecs before the chat path can need them again.
    app.GetAudioService().RestoreAudioModels();
    app.RestoreAudioRouting();
    ESP_LOGI(TAG, "audio exclusive OFF");
}

void RadioPage::StreamTask(void* arg) {
    auto* self = static_cast<RadioPage*>(arg);

    ESP_LOGI(TAG, "StreamTask start heap=%u stack=%d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), kTaskStack);

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
            xSemaphoreGive(self->stream_done_);
            vTaskDelete(nullptr);
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
        xSemaphoreGive(self->stream_done_);
        vTaskDelete(nullptr);
        return;
    }

    ctx.pcm.resize(kPcmOutInit);
    ctx.resample.resize(kResampleChunk);
    ESP_LOGI(TAG, "buffers ready, heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

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
    self->play_state_.store(RadioPlayState::Idle);
    xSemaphoreGive(self->stream_done_);
    vTaskDelete(nullptr);
}

void RadioPage::StartStream() {
    if (stream_task_ != nullptr) {
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
    // Drop a token left behind by a join that timed out, so the next StopStream
    // cannot return while this new task is still alive.
    xSemaphoreTake(stream_done_, 0);

    // Take the codec here, on the main loop, before allocating the stack: tearing
    // down the wake-word/AFE pipeline releases a large block of internal SRAM, and
    // this board does not have enough free to carve out the task stack without it.
    // Capture/release now pair on the same thread (StartStream / StopStream).
    ESP_LOGI(TAG, "heap before capture=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    CaptureAudioExclusive();
    ESP_LOGI(TAG, "heap after capture=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    stream_run_.store(true);
    user_paused_.store(false);
    play_state_.store(RadioPlayState::Connecting);
    level_.store(0.0f);
    SetStatusHint("connecting");

    const BaseType_t ok = xTaskCreatePinnedToCore(StreamTask, "radio_stream", kTaskStack, this,
                                                  kTaskPrio, &stream_task_, kTaskCore);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream task create failed heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        stream_run_.store(false);
        stream_task_ = nullptr;
        play_state_.store(RadioPlayState::Error);
        SetStatusHint("low memory");
        ReleaseAudioExclusive();
    }
}

void RadioPage::StopStream() {
    if (stream_task_ == nullptr) {
        ReleaseAudioExclusive();
        return;
    }
    ESP_LOGI(TAG, "StopStream begin");
    stream_run_.store(false);
    user_paused_.store(false);

    // Bounded join. Reads use a 6s socket timeout and the decode loop checks
    // stream_run_ every frame, so this normally returns in well under a second.
    if (xSemaphoreTake(stream_done_, pdMS_TO_TICKS(4000)) != pdTRUE) {
        // Never vTaskDelete() here: the task may be mid-syscall (or already gone),
        // and killing it would leak its socket and leave driver mutexes locked.
        ESP_LOGE(TAG, "stream task did not exit in 4s; leaving it to finish");
    }
    stream_task_ = nullptr;
    play_state_.store(RadioPlayState::Idle);
    level_.store(0.0f);
    ReleaseAudioExclusive();
    ESP_LOGI(TAG, "StopStream end heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    const float lvl = level_.load();
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

    const int plot_w = kBarCount * (kBarW + kBarGap) - kBarGap;
    const int plot_x = (kScreenW - plot_w) / 2;
    for (int i = 0; i < kBarCount; ++i) {
        if (bars_[i] == nullptr) {
            continue;
        }
        float wave = lvl * (0.55f + 0.45f * std::sin(lvl * 12.0f + i * 0.7f));
        if (st != RadioPlayState::Playing) {
            wave *= 0.15f;
        }
        int h = static_cast<int>(wave * (kPlotH - 2));
        if (h < 2) {
            h = 2;
        }
        if (h > kPlotH) {
            h = kPlotH;
        }
        lv_obj_set_size(bars_[i], kBarW, h);
        lv_obj_set_pos(bars_[i], plot_x + i * (kBarW + kBarGap), kPlotTop + kPlotH - h);
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex((i % 5 == 0) ? 0x00FFFF : 0x00FF66), 0);
    }
}

void RadioPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    if (station_index_.load() < 0 || station_index_.load() >= kStationCount) {
        station_index_.store(kDefaultStation);
    }
    ESP_LOGI(TAG, "OnEnter station=%d (%s) heap=%u", station_index_.load(),
             kStations[station_index_.load()].short_name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

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
    Application::GetInstance().Schedule([this]() {
        if (active_) {
            StartStream();
        }
    });
}

void RadioPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
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
