#include "radio_page.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include "esp_audio_simple_dec.h"
#include "impl/esp_aac_dec.h"
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
    const char* playlist_url;
};

// Both verified working (HLS MPEG-TS AAC).
constexpr Station kStations[] = {
    {"News", "CNR1 Voice of China", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"},
    {"Music", "FM974 Beijing Music", "https://brtv-radiolive.rbc.cn/alive/fm974.m3u8"},
};
constexpr int kStationCount = static_cast<int>(sizeof(kStations) / sizeof(kStations[0]));

constexpr int kHttpChunk = 2048;
constexpr int kPcmOutInit = 8192;
constexpr int kTaskStack = 12288;
constexpr int kTaskPrio = 5;
constexpr int kDefaultVolume = 70;

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

void ConfigureHttpClient(esp_http_client_config_t& cfg, const char* url, int timeout_ms) {
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.method = HTTP_METHOD_GET;
    cfg.buffer_size = 1024;
    cfg.user_agent = "xiaozhi-ADV-car/radio";
    // HTTPS (FM974) needs the system CA bundle; harmless for plain HTTP.
    if (url != nullptr && strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
}

bool HttpGetText(const char* url, std::string& out, int timeout_ms = 8000) {
    out.clear();
    esp_http_client_config_t cfg = {};
    ConfigureHttpClient(cfg, url, timeout_ms);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP open fail %s err=%s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buf[512];
    while (true) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, n);
        if (out.size() > 8192) {
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return !out.empty();
}

struct StreamCtx {
    RadioPage* page = nullptr;
    esp_audio_simple_dec_handle_t dec = nullptr;
    std::vector<uint8_t> pcm;
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    float ema_level = 0.0f;
    uint32_t pcm_chunks = 0;
};

void FeedPcmToSpeaker(StreamCtx* ctx, const uint8_t* data, uint32_t bytes) {
    if (data == nullptr || bytes < 4 || ctx == nullptr || ctx->page == nullptr) {
        return;
    }
    if (ctx->page->IsUserPaused()) {
        return;
    }
    if (ctx->sample_rate == 0 || ctx->channels == 0) {
        esp_audio_simple_dec_info_t info {};
        if (esp_audio_simple_dec_get_info(ctx->dec, &info) == ESP_AUDIO_ERR_OK &&
            info.sample_rate > 0 && info.channel > 0) {
            ctx->sample_rate = info.sample_rate;
            ctx->channels = info.channel;
            ESP_LOGI(TAG, "stream info rate=%u ch=%u", (unsigned)ctx->sample_rate,
                     (unsigned)ctx->channels);
        } else {
            // Decoder not ready yet  drop until info available.
            return;
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
    const int in_frames = static_cast<int>(bytes / sizeof(int16_t) / ctx->channels);
    if (in_frames <= 0) {
        return;
    }

    // Target board output: 24 kHz mono (Cardputer ADV).
    const int out_rate = codec->output_sample_rate();
    const int src_rate = static_cast<int>(ctx->sample_rate);
    int step = 1;
    if (src_rate > out_rate && out_rate > 0) {
        step = src_rate / out_rate;
        if (step < 1) {
            step = 1;
        }
    }

    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(in_frames / step) + 8);

    double energy = 0;
    int energy_n = 0;
    for (int i = 0; i < in_frames; i += step) {
        int32_t sample = 0;
        if (ctx->channels >= 2) {
            sample = (static_cast<int32_t>(in[i * ctx->channels]) +
                      static_cast<int32_t>(in[i * ctx->channels + 1])) /
                     2;
        } else {
            sample = in[i * ctx->channels];
        }
        out.push_back(static_cast<int16_t>(sample));
        energy += static_cast<double>(sample) * static_cast<double>(sample);
        ++energy_n;
    }

    if (!out.empty()) {
        codec->OutputData(out);
        ++ctx->pcm_chunks;
        if (ctx->pcm_chunks == 1 || (ctx->pcm_chunks % 50) == 0) {
            ESP_LOGI(TAG, "pcm out chunks=%u samples=%u vol=%d out_en=%d",
                     (unsigned)ctx->pcm_chunks, (unsigned)out.size(), codec->output_volume(),
                     codec->output_enabled() ? 1 : 0);
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

    while (raw.consumed < raw.len) {
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
            if (need > 48 * 1024) {
                need = 48 * 1024;
            }
            ctx->pcm.resize(need);
            continue;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGD(TAG, "dec process ret=%d", (int)ret);
            break;
        }
        if (frame.decoded_size > 0) {
            FeedPcmToSpeaker(ctx, frame.buffer, frame.decoded_size);
        }
        if (raw.consumed == before) {
            break;
        }
    }
    return ESP_OK;
}

bool PlayTsSegment(StreamCtx* ctx, const std::string& url) {
    esp_http_client_config_t cfg = {};
    ConfigureHttpClient(cfg, url.c_str(), 12000);
    cfg.buffer_size = kHttpChunk;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "segment HTTP %d %s", status, url.c_str());
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::vector<uint8_t> chunk(kHttpChunk);
    bool ok = false;
    while (ctx->page != nullptr && ctx->page->IsStreamRunning()) {
        if (ctx->page->IsUserPaused()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int n = esp_http_client_read(client, reinterpret_cast<char*>(chunk.data()), kHttpChunk);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            ok = true;
            break;
        }
        DecodeChunk(ctx, chunk.data(), n);
        ok = true;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool ParsePlaylist(const std::string& body, const std::string& base_url,
                   std::vector<std::string>& segs, uint32_t& media_seq) {
    segs.clear();
    media_seq = 0;
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
        if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
            segs.push_back(line);
        } else {
            segs.push_back(base_url + line);
        }
    }
    // Some live CDNs omit MEDIA-SEQUENCE; still playable if we have segments.
    return !segs.empty();
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
    lv_label_set_text(vol_label_, "vol70");
    lv_obj_set_style_text_font(vol_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(vol_label_, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(vol_label_, LV_ALIGN_TOP_RIGHT, -6, 4);

    station_label_ = lv_label_create(panel_);
    lv_label_set_text(station_label_, kStations[0].title);
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
    // Pause xiaozhi mic path so Radio owns the speaker exclusively.
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(false);
    audio.ResetDecoder();
    audio.SetExternalPlaybackActive(true);
    audio_exclusive_ = true;

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        if (codec->output_volume() < kDefaultVolume) {
            codec->SetOutputVolume(kDefaultVolume);
        }
        codec->EnableOutput(true);
        ESP_LOGI(TAG, "audio exclusive ON vol=%d out=%d", codec->output_volume(),
                 codec->output_enabled() ? 1 : 0);
    }
}

void RadioPage::ReleaseAudioExclusive() {
    if (!audio_exclusive_) {
        // Still clear hold + restore in case of partial enter failure.
        Application::GetInstance().GetAudioService().SetExternalPlaybackActive(false);
        Application::GetInstance().RestoreAudioRouting();
        return;
    }
    audio_exclusive_ = false;
    auto& app = Application::GetInstance();
    app.GetAudioService().SetExternalPlaybackActive(false);
    // Always re-sync Chat/TTS wake-word path from device state (not saved flags).
    app.RestoreAudioRouting();
    ESP_LOGI(TAG, "audio exclusive OFF + RestoreAudioRouting");
}

void RadioPage::StreamTask(void* arg) {
    auto* self = static_cast<RadioPage*>(arg);
    StreamCtx ctx;
    ctx.page = self;

    static bool codecs_ready = false;
    if (!codecs_ready) {
        if (esp_aac_dec_register() != ESP_AUDIO_ERR_OK || esp_ts_dec_register() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "AAC/TS register failed");
            self->play_state_.store(RadioPlayState::Error);
            self->stream_task_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        codecs_ready = true;
    }

    esp_ts_dec_cfg_t ts_cfg = {.aac_plus_enable = false};
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS,
        .dec_cfg = &ts_cfg,
        .cfg_size = sizeof(ts_cfg),
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&dec_cfg, &ctx.dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "TS simple decoder open failed");
        self->play_state_.store(RadioPlayState::Error);
        self->stream_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    uint32_t last_seq = 0;
    bool have_last = false;
    int fail_streak = 0;
    uint32_t bound_gen = self->StationGeneration();
    int bound_station = self->StationIndex();

    while (self->stream_run_.load()) {
        // Station switch: reset live cursor.
        uint32_t gen = self->StationGeneration();
        int st = self->StationIndex();
        if (gen != bound_gen || st != bound_station) {
            bound_gen = gen;
            bound_station = st;
            have_last = false;
            last_seq = 0;
            fail_streak = 0;
            esp_audio_simple_dec_reset(ctx.dec);
            ctx.sample_rate = 0;
            ctx.channels = 0;
            ctx.pcm_chunks = 0;
            ESP_LOGI(TAG, "station switch -> %d %s", st, kStations[st].short_name);
        }

        if (self->IsUserPaused()) {
            self->play_state_.store(RadioPlayState::Paused);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        if (bound_station < 0 || bound_station >= kStationCount) {
            bound_station = 0;
        }
        const Station& station = kStations[bound_station];
        const std::string base = PlaylistBaseUrl(station.playlist_url);

        self->play_state_.store(RadioPlayState::Connecting);
        std::string playlist;
        if (!HttpGetText(station.playlist_url, playlist)) {
            fail_streak++;
            if (fail_streak > 5) {
                self->play_state_.store(RadioPlayState::Error);
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        std::vector<std::string> segs;
        uint32_t media_seq = 0;
        if (!ParsePlaylist(playlist, base, segs, media_seq)) {
            fail_streak++;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t start = 0;
        if (have_last) {
            if (media_seq + segs.size() <= last_seq + 1) {
                vTaskDelay(pdMS_TO_TICKS(800));
                continue;
            }
            uint32_t want = last_seq + 1;
            if (want >= media_seq) {
                start = want - media_seq;
            } else {
                start = segs.size() > 1 ? segs.size() - 1 : 0;
            }
        } else {
            start = segs.size() > 1 ? segs.size() - 1 : 0;
        }

        for (size_t i = start; i < segs.size() && self->stream_run_.load(); ++i) {
            if (self->StationGeneration() != bound_gen || self->IsUserPaused()) {
                break;
            }
            self->play_state_.store(RadioPlayState::Connecting);
            ESP_LOGI(TAG, "play segment %s", segs[i].c_str());
            // Soft reset between segments (keep codec type); refresh stream info.
            esp_audio_simple_dec_reset(ctx.dec);
            ctx.sample_rate = 0;
            ctx.channels = 0;
            if (PlayTsSegment(&ctx, segs[i])) {
                self->play_state_.store(RadioPlayState::Playing);
                last_seq = media_seq + static_cast<uint32_t>(i);
                have_last = true;
                fail_streak = 0;
            } else {
                fail_streak++;
                if (fail_streak > 5) {
                    self->play_state_.store(RadioPlayState::Error);
                }
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ctx.dec != nullptr) {
        esp_audio_simple_dec_close(ctx.dec);
        ctx.dec = nullptr;
    }

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
    BaseType_t ok = xTaskCreatePinnedToCore(StreamTask, "cnr_radio", kTaskStack, this, kTaskPrio,
                                              &stream_task_, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "stream task create failed heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        stream_run_.store(false);
        stream_task_ = nullptr;
        play_state_.store(RadioPlayState::Error);
    }
}

void RadioPage::StopStream() {
    stream_run_.store(false);
    user_paused_.store(false);
    for (int i = 0; i < 50 && stream_task_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (stream_task_ != nullptr) {
        ESP_LOGW(TAG, "force-delete stream task");
        vTaskDelete(stream_task_);
        stream_task_ = nullptr;
    }
    play_state_.store(RadioPlayState::Idle);
    level_.store(0.0f);
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
        if (st == RadioPlayState::Error) {
            lv_label_set_text(status_label_, "WiFi/CDN fail");
        } else {
            lv_label_set_text(status_label_,
                              st_idx == 0 ? "[News] Music  ,/ / N/M"
                                          : "News [Music]  ,/ / N/M");
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
    ESP_LOGI(TAG, "OnEnter radio station=%d heap=%u", station_index_.load(),
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
    }
    display->HideChatUi();
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

    // Station: 1/N/, = News ; 2/M/ = Music
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
    // Play/pause
    if (event.key_code == KC_P || event.key_code == KC_SPACE || ch == 'p' || ch == ' ') {
        TogglePause();
        return true;
    }
    // Volume ; up / . down
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
