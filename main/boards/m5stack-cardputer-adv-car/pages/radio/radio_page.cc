#include "radio_page.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include "esp_audio_simple_dec.h"
#include "impl/esp_aac_dec.h"
#include "impl/esp_mp3_dec.h"
#include "impl/esp_ts_dec.h"

#include <esp_crt_bundle.h>
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

struct Station {
    const char* short_name;
    const char* title;
    const char* mp3_url;   // preferred: continuous HTTP MP3 (no HLS/TS)
    const char* hls_url;   // fallback MPEG-TS AAC
};

// Prefer qtfm HTTP MP3 (works in browser/curl; avoids HTTPS+HLS heap on no-PSRAM).
// HLS kept as fallback (official FM974 / CNR CDN).
constexpr Station kStations[] = {
    {"News", "CNR1 Voice of China", "http://lhttp.qtfm.cn/live/15318317/64k.mp3",
     "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"Music", "FM974 Beijing Music", "http://lhttp.qtfm.cn/live/332/64k.mp3",
     "https://brtv-radiolive.rbc.cn/alive/fm974.m3u8"},
};
constexpr int kStationCount = static_cast<int>(sizeof(kStations) / sizeof(kStations[0]));
constexpr int kDefaultStation = 1;  // Music / FM974

constexpr int kHttpChunk = 1536;
constexpr int kPcmOutMax = 16 * 1024;
constexpr int kPcmOutInit = 4096;
constexpr int kPlaylistMax = 4096;
constexpr int kTaskStack = 12288;
constexpr int kTaskPrio = 3;
constexpr int kDefaultVolume = 80;
constexpr int kHttpTimeoutMs = 8000;
constexpr int kMp3TimeoutMs = 12000;
constexpr int kSegmentTimeoutMs = 8000;
constexpr int kMaxSegmentBytes = 320 * 1024;

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

std::string PlaylistBaseUrl(const char* playlist_url) {
    std::string u = playlist_url ? playlist_url : "";
    auto slash = u.rfind('/');
    if (slash == std::string::npos) {
        return u;
    }
    return u.substr(0, slash + 1);
}

void ConfigureHttpClient(esp_http_client_config_t& cfg, const char* url, int timeout_ms,
                         bool insecure_tls) {
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = kHttpChunk;
    cfg.buffer_size_tx = 512;
    cfg.user_agent = "Mozilla/5.0 (xiaozhi-ADV-car/radio)";
    cfg.max_redirection_count = 5;
    cfg.disable_auto_redirect = false;
    cfg.keep_alive_enable = false;
    if (url != nullptr && strncmp(url, "https://", 8) == 0) {
        if (insecure_tls) {
            cfg.crt_bundle_attach = nullptr;
            cfg.skip_cert_common_name_check = true;
        } else {
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.skip_cert_common_name_check = false;
        }
    }
}

bool StatusOk(int status) {
    return status >= 200 && status < 300;
}

bool FollowRedirectIfNeeded(esp_http_client_handle_t client, esp_err_t& err) {
    int status = esp_http_client_get_status_code(client);
    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        return true;
    }
    if (esp_http_client_set_redirection(client) != ESP_OK) {
        return false;
    }
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    return true;
}

bool HttpGetTextOnce(RadioPage* page, const char* url, std::string& out, int timeout_ms,
                     bool insecure_tls) {
    out.clear();
    if (page != nullptr && !page->IsStreamRunning()) {
        return false;
    }

    esp_http_client_config_t cfg = {};
    ConfigureHttpClient(cfg, url, timeout_ms, insecure_tls);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGW(TAG, "HTTP init fail %s", url);
        return false;
    }

    ESP_LOGI(TAG, "HTTP open playlist insecure=%d %s", insecure_tls ? 1 : 0, url);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open fail %s err=%s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int content_len = esp_http_client_fetch_headers(client);
    if (!FollowRedirectIfNeeded(client, err)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (!StatusOk(status)) {
        ESP_LOGW(TAG, "HTTP %d cl=%d for %s", status, content_len, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buf[256];
    while (page == nullptr || page->IsStreamRunning()) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        out.append(buf, n);
        if (out.size() > kPlaylistMax) {
            break;
        }
        taskYIELD();
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP GET ok %uB %s", static_cast<unsigned>(out.size()), url);
    return !out.empty();
}

bool HttpGetText(RadioPage* page, const char* url, std::string& out, int timeout_ms = kHttpTimeoutMs) {
    if (HttpGetTextOnce(page, url, out, timeout_ms, false)) {
        return true;
    }
    if (url != nullptr && strncmp(url, "https://", 8) == 0) {
        if (page != nullptr && !page->IsStreamRunning()) {
            return false;
        }
        return HttpGetTextOnce(page, url, out, timeout_ms, true);
    }
    return false;
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
    uint32_t decode_ok = 0;
    uint32_t decode_fail = 0;
};

void FeedPcmToSpeaker(StreamCtx* ctx, const uint8_t* data, uint32_t bytes) {
    if (data == nullptr || bytes < 4 || ctx == nullptr || ctx->page == nullptr) {
        return;
    }
    if (!ctx->page->IsStreamRunning() || ctx->page->IsUserPaused()) {
        return;
    }
    if (ctx->sample_rate == 0 || ctx->channels == 0) {
        esp_audio_simple_dec_info_t info {};
        if (esp_audio_simple_dec_get_info(ctx->dec, &info) == ESP_AUDIO_ERR_OK &&
            info.sample_rate > 0 && info.channel > 0) {
            ctx->sample_rate = info.sample_rate;
            ctx->channels = info.channel;
            ESP_LOGI(TAG, "stream info rate=%u ch=%u bits=%u", (unsigned)ctx->sample_rate,
                     (unsigned)ctx->channels, (unsigned)info.bits_per_sample);
        } else {
            // Still try to play assuming 48k stereo if info not ready — better than silence.
            ctx->sample_rate = 48000;
            ctx->channels = bytes >= 8 ? 2 : 1;
            ESP_LOGW(TAG, "stream info missing; assume rate=%u ch=%u", (unsigned)ctx->sample_rate,
                     (unsigned)ctx->channels);
        }
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    Application::GetInstance().GetAudioService().NotifyOutputActivity();

    const int16_t* in = reinterpret_cast<const int16_t*>(data);
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

    // Fractional resample (handles 44100?24000 and 48000?24000).
    int out_frames = static_cast<int>((static_cast<int64_t>(in_frames) * out_rate) / src_rate);
    if (out_frames < 1) {
        out_frames = 1;
    }
    ctx->resample.clear();
    ctx->resample.reserve(static_cast<size_t>(out_frames) + 8);

    double energy = 0;
    int energy_n = 0;
    for (int j = 0; j < out_frames; ++j) {
        int i = static_cast<int>((static_cast<int64_t>(j) * src_rate) / out_rate);
        if (i >= in_frames) {
            i = in_frames - 1;
        }
        int32_t sample = 0;
        if (ch >= 2) {
            sample = (static_cast<int32_t>(in[i * ch]) + static_cast<int32_t>(in[i * ch + 1])) / 2;
        } else {
            sample = in[i * ch];
        }
        ctx->resample.push_back(static_cast<int16_t>(sample));
        energy += static_cast<double>(sample) * static_cast<double>(sample);
        ++energy_n;
    }

    if (!ctx->resample.empty()) {
        codec->OutputData(ctx->resample);
        ++ctx->pcm_chunks;
        ctx->page->NotifyPlaying();
        if (ctx->pcm_chunks == 1 || (ctx->pcm_chunks % 50) == 0) {
            ESP_LOGI(TAG, "pcm out chunks=%u samples=%u rate=%u->%u vol=%d heap=%u",
                     (unsigned)ctx->pcm_chunks, (unsigned)ctx->resample.size(),
                     (unsigned)src_rate, (unsigned)out_rate, codec->output_volume(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }
    if (energy_n > 0) {
        float rms = static_cast<float>(std::sqrt(energy / energy_n) / 32768.0);
        ctx->ema_level = ctx->ema_level * 0.7f + rms * 0.3f;
        ctx->page->NotifyLevel(ctx->ema_level);
    }
}

esp_err_t DecodeChunk(StreamCtx* ctx, uint8_t* data, int len) {
    if (ctx->dec == nullptr || data == nullptr || len <= 0) {
        return ESP_OK;
    }
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = data;
    raw.len = static_cast<uint32_t>(len);
    raw.eos = false;
    raw.consumed = 0;

    int spins = 0;
    while (raw.consumed < raw.len) {
        if (ctx->page != nullptr && !ctx->page->IsStreamRunning()) {
            break;
        }
        if (ctx->pcm.size() < kPcmOutInit) {
            ctx->pcm.resize(kPcmOutInit);
        }
        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = ctx->pcm.data();
        frame.len = static_cast<uint32_t>(ctx->pcm.size());

        uint32_t before = raw.consumed;
        esp_audio_err_t ret = esp_audio_simple_dec_process(ctx->dec, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint32_t need = frame.needed_size > 0 ? frame.needed_size
                                                 : static_cast<uint32_t>(ctx->pcm.size() * 2);
            if (need > kPcmOutMax) {
                need = kPcmOutMax;
            }
            if (need <= ctx->pcm.size()) {
                ESP_LOGW(TAG, "dec need=%u but pcm already %u — drop", (unsigned)need,
                         (unsigned)ctx->pcm.size());
                break;
            }
            ctx->pcm.resize(need);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ++ctx->decode_fail;
            if ((ctx->decode_fail % 30) == 1) {
                ESP_LOGW(TAG, "dec process ret=%d consumed=%u/%u", (int)ret,
                         (unsigned)raw.consumed, (unsigned)raw.len);
            }
            // Skip one byte to resync rather than stall forever on bad frame.
            if (raw.consumed == before) {
                raw.consumed++;
            }
            continue;
        }
        if (frame.decoded_size > 0) {
            ++ctx->decode_ok;
            FeedPcmToSpeaker(ctx, frame.buffer, frame.decoded_size);
        }
        if (raw.consumed == before) {
            break;
        }
        if (++spins > 64) {
            taskYIELD();
            spins = 0;
        }
    }
    return ESP_OK;
}

bool OpenDecoder(StreamCtx* ctx, esp_audio_simple_dec_type_t type) {
    if (ctx->dec != nullptr) {
        esp_audio_simple_dec_close(ctx->dec);
        ctx->dec = nullptr;
    }
    esp_ts_dec_cfg_t ts_cfg = {.aac_plus_enable = true};
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = type;
    dec_cfg.use_frame_dec = false;
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_TS) {
        dec_cfg.dec_cfg = &ts_cfg;
        dec_cfg.cfg_size = sizeof(ts_cfg);
    } else {
        dec_cfg.dec_cfg = nullptr;
        dec_cfg.cfg_size = 0;
    }
    if (esp_audio_simple_dec_open(&dec_cfg, &ctx->dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed type=%u", (unsigned)type);
        ctx->dec = nullptr;
        return false;
    }
    ctx->sample_rate = 0;
    ctx->channels = 0;
    ESP_LOGI(TAG, "decoder open ok type=%u", (unsigned)type);
    return true;
}

bool PlayMp3Once(StreamCtx* ctx, const char* url, uint32_t bound_gen) {
    if (ctx->page == nullptr || url == nullptr || !ctx->page->IsStreamRunning()) {
        return false;
    }
    if (!OpenDecoder(ctx, ESP_AUDIO_SIMPLE_DEC_TYPE_MP3)) {
        ctx->page->SetStatusHint("mp3 decoder fail");
        return false;
    }

    esp_http_client_config_t cfg = {};
    ConfigureHttpClient(cfg, url, kMp3TimeoutMs, false);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ctx->page->SetStatusHint("http init fail");
        return false;
    }

    ESP_LOGI(TAG, "MP3 open %s heap=%u", url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ctx->page->SetStatusHint("mp3 connecting");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MP3 open fail err=%s", esp_err_to_name(err));
        ctx->page->SetStatusHint("mp3 open fail");
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    if (!FollowRedirectIfNeeded(client, err)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        ctx->page->SetStatusHint("mp3 redirect fail");
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (!StatusOk(status)) {
        ESP_LOGW(TAG, "MP3 HTTP %d", status);
        ctx->page->SetStatusHint("mp3 HTTP err");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::vector<uint8_t> chunk(kHttpChunk);
    uint32_t total = 0;
    uint32_t pcm_at_start = ctx->pcm_chunks;
    int idle_reads = 0;

    while (ctx->page->IsStreamRunning() && ctx->page->StationGeneration() == bound_gen) {
        if (ctx->page->IsUserPaused()) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        int n = esp_http_client_read(client, reinterpret_cast<char*>(chunk.data()), kHttpChunk);
        if (n < 0) {
            ESP_LOGW(TAG, "MP3 read err %d after %uB pcm=%u", n, (unsigned)total,
                     (unsigned)(ctx->pcm_chunks - pcm_at_start));
            break;
        }
        if (n == 0) {
            ++idle_reads;
            if (idle_reads > 3) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        idle_reads = 0;
        total += static_cast<uint32_t>(n);
        ctx->bytes_in += static_cast<uint32_t>(n);
        DecodeChunk(ctx, chunk.data(), n);
        if ((total % (kHttpChunk * 16)) == 0) {
            taskYIELD();
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const uint32_t pcm_delta = ctx->pcm_chunks - pcm_at_start;
    ESP_LOGI(TAG, "MP3 session bytes=%u pcm_chunks=%u dec_ok=%u dec_fail=%u", (unsigned)total,
             (unsigned)pcm_delta, (unsigned)ctx->decode_ok, (unsigned)ctx->decode_fail);
    if (pcm_delta == 0) {
        ctx->page->SetStatusHint("mp3 no PCM");
        return false;
    }
    return true;
}

bool PlayTsSegmentOnce(StreamCtx* ctx, const std::string& url, bool insecure_tls) {
    if (ctx->page == nullptr || !ctx->page->IsStreamRunning()) {
        return false;
    }

    esp_http_client_config_t cfg = {};
    ConfigureHttpClient(cfg, url.c_str(), kSegmentTimeoutMs, insecure_tls);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    ESP_LOGI(TAG, "segment open insecure=%d %s", insecure_tls ? 1 : 0, url.c_str());
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    if (!FollowRedirectIfNeeded(client, err)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (!StatusOk(status)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::vector<uint8_t> chunk(kHttpChunk);
    bool ok = false;
    uint32_t total = 0;
    uint32_t reads = 0;
    while (ctx->page != nullptr && ctx->page->IsStreamRunning()) {
        if (ctx->page->IsUserPaused()) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        int n = esp_http_client_read(client, reinterpret_cast<char*>(chunk.data()), kHttpChunk);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            ok = total > 0;
            break;
        }
        total += static_cast<uint32_t>(n);
        ctx->bytes_in += static_cast<uint32_t>(n);
        DecodeChunk(ctx, chunk.data(), n);
        ok = true;
        ++reads;
        if ((reads % 8) == 0) {
            taskYIELD();
        }
        if (total > kMaxSegmentBytes) {
            ok = true;
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "segment done ok=%d bytes=%u pcm_chunks=%u", ok ? 1 : 0, (unsigned)total,
             (unsigned)ctx->pcm_chunks);
    return ok;
}

bool PlayTsSegment(StreamCtx* ctx, const std::string& url) {
    if (PlayTsSegmentOnce(ctx, url, false)) {
        return true;
    }
    if (ctx->page != nullptr && !ctx->page->IsStreamRunning()) {
        return false;
    }
    if (url.rfind("https://", 0) == 0) {
        return PlayTsSegmentOnce(ctx, url, true);
    }
    return false;
}

bool ResolveUri(const std::string& base_url, const std::string& line, std::string& out) {
    if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
        out = line;
        return true;
    }
    out = base_url + line;
    return !out.empty();
}

bool ParsePlaylist(const std::string& body, const std::string& base_url,
                   std::vector<std::string>& segs, uint32_t& media_seq,
                   std::string* nested_playlist) {
    segs.clear();
    media_seq = 0;
    if (nested_playlist != nullptr) {
        nested_playlist->clear();
    }
    bool is_master = body.find("#EXT-X-STREAM-INF") != std::string::npos;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos = end + 1;

        if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            media_seq = static_cast<uint32_t>(std::strtoul(line.c_str() + 22, nullptr, 10));
            continue;
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::string uri;
        if (!ResolveUri(base_url, line, uri)) {
            continue;
        }
        if (is_master && nested_playlist != nullptr && nested_playlist->empty()) {
            *nested_playlist = uri;
            return false;
        }
        segs.push_back(uri);
    }
    return !segs.empty();
}

bool PlayHlsCycle(StreamCtx* ctx, const Station& station, uint32_t bound_gen, uint32_t& last_seq,
                  bool& have_last) {
    if (station.hls_url == nullptr || ctx->page == nullptr) {
        return false;
    }
    if (!OpenDecoder(ctx, ESP_AUDIO_SIMPLE_DEC_TYPE_TS)) {
        ctx->page->SetStatusHint("ts decoder fail");
        return false;
    }
    ctx->page->SetStatusHint("hls connecting");

    const std::string base = PlaylistBaseUrl(station.hls_url);
    std::string playlist;
    std::string nested;
    if (!HttpGetText(ctx->page, station.hls_url, playlist)) {
        ctx->page->SetStatusHint("hls playlist fail");
        return false;
    }

    std::vector<std::string> segs;
    uint32_t media_seq = 0;
    if (!ParsePlaylist(playlist, base, segs, media_seq, &nested)) {
        if (!nested.empty()) {
            const std::string nested_base = PlaylistBaseUrl(nested.c_str());
            std::string media_body;
            if (!HttpGetText(ctx->page, nested.c_str(), media_body) ||
                !ParsePlaylist(media_body, nested_base, segs, media_seq, nullptr)) {
                ctx->page->SetStatusHint("hls variant fail");
                return false;
            }
        } else {
            ctx->page->SetStatusHint("hls empty");
            return false;
        }
    }

    size_t start = 0;
    if (have_last) {
        if (media_seq + segs.size() <= last_seq + 1) {
            vTaskDelay(pdMS_TO_TICKS(400));
            return true;  // waiting for next segment is OK
        }
        uint32_t want = last_seq + 1;
        if (want >= media_seq) {
            start = want - media_seq;
        } else {
            start = segs.size() > 1 ? segs.size() - 1 : 0;
        }
    } else {
        start = segs.size() > 2 ? segs.size() - 2 : 0;
    }

    bool any = false;
    for (size_t i = start; i < segs.size() && ctx->page->IsStreamRunning(); ++i) {
        if (ctx->page->StationGeneration() != bound_gen || ctx->page->IsUserPaused()) {
            break;
        }
        if (PlayTsSegment(ctx, segs[i])) {
            last_seq = media_seq + static_cast<uint32_t>(i);
            have_last = true;
            any = true;
        } else {
            break;
        }
        taskYIELD();
    }
    if (!any && ctx->pcm_chunks == 0) {
        ctx->page->SetStatusHint("hls no PCM");
    }
    return any || ctx->pcm_chunks > 0;
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

bool RadioPage::IsStreamRunning() const {
    return stream_run_.load();
}

bool RadioPage::IsUserPaused() const {
    return user_paused_.load();
}

int RadioPage::StationIndex() const {
    return station_index_.load();
}

uint32_t RadioPage::StationGeneration() const {
    return station_gen_.load();
}

void RadioPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    status_label_ = nullptr;
    station_label_ = nullptr;
    listening_label_ = nullptr;
    play_label_ = nullptr;
    vol_label_ = nullptr;
    for (int i = 0; i < kBarCount; ++i) {
        bars_[i] = nullptr;
    }
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
    lv_label_set_text(vol_label_, "vol80");
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
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(false);
    audio.ResetDecoder();
    audio.SetExternalPlaybackActive(true);
    audio_exclusive_ = true;

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(kDefaultVolume);
        codec->EnableOutput(true);
        ESP_LOGI(TAG, "audio exclusive ON vol=%d out=%d", codec->output_volume(),
                 codec->output_enabled() ? 1 : 0);
    }
}

void RadioPage::ReleaseAudioExclusive() {
    if (!audio_exclusive_) {
        Application::GetInstance().GetAudioService().SetExternalPlaybackActive(false);
        Application::GetInstance().RestoreAudioRouting();
        return;
    }
    audio_exclusive_ = false;
    auto& app = Application::GetInstance();
    app.GetAudioService().SetExternalPlaybackActive(false);
    app.RestoreAudioRouting();
    ESP_LOGI(TAG, "audio exclusive OFF + RestoreAudioRouting");
}

void RadioPage::StreamTask(void* arg) {
    auto* self = static_cast<RadioPage*>(arg);
    StreamCtx ctx;
    ctx.page = self;
    ctx.pcm.reserve(kPcmOutInit);
    ctx.resample.reserve(1024);

    ESP_LOGI(TAG, "StreamTask start heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    static bool codecs_ready = false;
    if (!codecs_ready) {
        bool ok = true;
        if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "MP3 register failed");
            ok = false;
        }
        if (esp_aac_dec_register() != ESP_AUDIO_ERR_OK || esp_ts_dec_register() != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AAC/TS register failed (HLS fallback unavailable)");
        }
        if (!ok) {
            self->play_state_.store(RadioPlayState::Error);
            self->SetStatusHint("codec register fail");
            self->stream_task_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        codecs_ready = true;
        ESP_LOGI(TAG, "MP3(+AAC/TS) codecs registered");
    }

    int fail_streak = 0;
    uint32_t bound_gen = self->StationGeneration();
    int bound_station = self->StationIndex();
    uint32_t hls_last_seq = 0;
    bool hls_have_last = false;

    while (self->stream_run_.load()) {
        uint32_t gen = self->StationGeneration();
        int st = self->StationIndex();
        if (gen != bound_gen || st != bound_station) {
            bound_gen = gen;
            bound_station = st;
            fail_streak = 0;
            hls_have_last = false;
            hls_last_seq = 0;
            ctx.pcm_chunks = 0;
            ctx.bytes_in = 0;
            ctx.decode_ok = 0;
            ctx.decode_fail = 0;
            ESP_LOGI(TAG, "station switch -> %d %s", st, kStations[st].short_name);
        }

        if (self->IsUserPaused()) {
            self->play_state_.store(RadioPlayState::Paused);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (bound_station < 0 || bound_station >= kStationCount) {
            bound_station = 0;
        }
        const Station& station = kStations[bound_station];
        self->play_state_.store(RadioPlayState::Connecting);

        bool ok = false;
        // 1) Prefer continuous MP3 (HTTP) — rock-solid on no-PSRAM.
        if (station.mp3_url != nullptr && self->stream_run_.load()) {
            ESP_LOGI(TAG, "try MP3 %s", station.mp3_url);
            ok = PlayMp3Once(&ctx, station.mp3_url, bound_gen);
        }
        // 2) HLS fallback if MP3 failed and we still want this station.
        if (!ok && station.hls_url != nullptr && self->stream_run_.load() &&
            self->StationGeneration() == bound_gen && !self->IsUserPaused()) {
            ESP_LOGW(TAG, "MP3 failed/ended — try HLS %s", station.hls_url);
            ok = PlayHlsCycle(&ctx, station, bound_gen, hls_last_seq, hls_have_last);
        }

        if (!self->stream_run_.load()) {
            break;
        }
        if (self->StationGeneration() != bound_gen) {
            continue;
        }

        if (ok) {
            fail_streak = 0;
            // MP3 sessions end on read timeout — reconnect promptly.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        fail_streak++;
        ESP_LOGW(TAG, "play fail streak=%d station=%s", fail_streak, station.short_name);
        if (fail_streak > 3) {
            self->play_state_.store(RadioPlayState::Error);
            if (self->status_hint_[0] == '\0') {
                self->SetStatusHint("stream fail");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    if (ctx.dec != nullptr) {
        esp_audio_simple_dec_close(ctx.dec);
        ctx.dec = nullptr;
    }

    ESP_LOGI(TAG, "StreamTask exit pcm_chunks=%u bytes_in=%u", (unsigned)ctx.pcm_chunks,
             (unsigned)ctx.bytes_in);
    self->play_state_.store(RadioPlayState::Idle);
    self->stream_task_ = nullptr;
    vTaskDelete(nullptr);
}

void RadioPage::StartStream() {
    if (stream_task_ != nullptr) {
        return;
    }
    stream_run_.store(true);
    user_paused_.store(false);
    play_state_.store(RadioPlayState::Connecting);
    level_.store(0.0f);
    SetStatusHint("connecting");
    BaseType_t ok = xTaskCreatePinnedToCore(StreamTask, "radio_stream", kTaskStack, this, kTaskPrio,
                                              &stream_task_, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream task create failed heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        stream_run_.store(false);
        stream_task_ = nullptr;
        play_state_.store(RadioPlayState::Error);
        SetStatusHint("task create fail");
    } else {
        ESP_LOGI(TAG, "stream task created");
    }
}

void RadioPage::StopStream() {
    ESP_LOGI(TAG, "StopStream begin task=%p", stream_task_);
    stream_run_.store(false);
    user_paused_.store(false);
    for (int i = 0; i < 200 && stream_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (stream_task_ != nullptr) {
        ESP_LOGW(TAG, "stream task stuck — last-resort delete");
        vTaskDelete(stream_task_);
        stream_task_ = nullptr;
    }
    play_state_.store(RadioPlayState::Idle);
    level_.store(0.0f);
    ESP_LOGI(TAG, "StopStream end");
}

void RadioPage::SelectStation(int index) {
    if (index < 0 || index >= kStationCount) {
        return;
    }
    if (station_index_.load() == index) {
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
    bool next = !user_paused_.load();
    user_paused_.store(next);
    play_state_.store(next ? RadioPlayState::Paused : RadioPlayState::Connecting);
    ESP_LOGI(TAG, "TogglePause -> %s", next ? "paused" : "play");
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
    ESP_LOGI(TAG, "volume=%d", vol);
}

void RadioPage::UpdateUi(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr) {
        return;
    }

    RadioPlayState st = play_state_.load();
    float lvl = level_.load();
    int st_idx = station_index_.load();
    if (st_idx < 0 || st_idx >= kStationCount) {
        st_idx = 0;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    int vol = codec != nullptr ? codec->output_volume() : 0;

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
        if (st == RadioPlayState::Error && status_hint_[0] != '\0') {
            lv_label_set_text(status_label_, status_hint_);
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFF5555), 0);
        } else if (st == RadioPlayState::Connecting && status_hint_[0] != '\0') {
            lv_label_set_text(status_label_, status_hint_);
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0xAAAAAA), 0);
        } else {
            lv_label_set_text(status_label_,
                              st_idx == 0 ? "[News] Music  ,/ / N/M"
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
        uint32_t color = (i % 5 == 0) ? 0x00FFFF : 0x00FF66;
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(color), 0);
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
    ESP_LOGI(TAG, "OnEnter radio station=%d (%s) heap=%u", station_index_.load(),
             kStations[station_index_.load()].short_name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    CaptureAudioExclusive();
    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        ReleaseAudioExclusive();
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
        if (listening_label_ != nullptr) {
            lv_label_set_text(listening_label_, "connecting");
            lv_obj_set_style_text_color(listening_label_, lv_color_hex(0xFFAA00), 0);
        }
    }
    display->HideChatUi();
    user_paused_.store(false);
    StartStream();
}

void RadioPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    StopStream();
    ReleaseAudioExclusive();
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
    const char ch = event.key_char ? static_cast<char>(std::tolower(static_cast<unsigned char>(event.key_char[0]))) : '\0';

    if (event.key_code == KC_1 || event.key_code == KC_N || event.key_code == KC_COMMA || ch == '1' ||
        ch == 'n' || ch == ',') {
        SelectStation(0);
        return true;
    }
    if (event.key_code == KC_2 || event.key_code == KC_M || event.key_code == KC_SLASH || ch == '2' ||
        ch == 'm' || ch == '/') {
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
