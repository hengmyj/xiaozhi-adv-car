#include "cursor_page.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <vector>

#define TAG "CursorPage"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr lv_coord_t kHeaderH = 22;
constexpr lv_coord_t kFooterH = 18;
constexpr lv_coord_t kPlotTop = 24;
constexpr lv_coord_t kPlotH = 78;
constexpr lv_coord_t kBarW = 7;
constexpr lv_coord_t kBarGap = 2;

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

}  // namespace

void CursorPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    for (int i = 0; i < kBarCount; ++i) {
        bars_[i] = nullptr;
        levels_[i] = 0;
    }
}

void CursorPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
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

    // Header: Music | Mic | battery icon (mic visualizer; real Cursor later)
    lv_obj_t* title = lv_label_create(panel_);
    lv_label_set_text(title, "Music");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(title, 6, 2);

    lv_obj_t* mode = lv_label_create(panel_);
    lv_label_set_text(mode, "Mic");
    lv_obj_set_style_text_font(mode, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mode, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(mode, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t* batt = lv_label_create(panel_);
    lv_label_set_text(batt, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(batt, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(batt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(batt, LV_ALIGN_TOP_RIGHT, -6, 3);

    // Plot area with vertical grid lines + bars.
    const int plot_w = kBarCount * (kBarW + kBarGap) - kBarGap;
    const int plot_x = (kScreenW - plot_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        int x = plot_x + i * (kBarW + kBarGap);
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
        levels_[i] = 0;
    }

    // X labels 0..24 every 3
    for (int v = 0; v <= 24; v += 3) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", v);
        lv_obj_t* lbl = lv_label_create(panel_);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        int idx = (v * (kBarCount - 1)) / 24;
        int x = plot_x + idx * (kBarW + kBarGap);
        lv_obj_set_pos(lbl, x - 2, kPlotTop + kPlotH + 1);
    }

    // Footer hint
    lv_obj_t* hint = lv_label_create(panel_);
    lv_label_set_text(hint, "play music near mic");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    (void)kHeaderH;
    (void)kFooterH;
}

void CursorPage::FreeMicBuffer() {
    if (mic_buf_.empty() && mic_buf_.capacity() == 0) {
        return;
    }
    const size_t bytes = mic_buf_.capacity() * sizeof(int16_t);
    std::vector<int16_t>().swap(mic_buf_);
    ESP_LOGI(TAG, "mic_buf freed %uB", (unsigned)bytes);
}

void CursorPage::ReleaseMicExclusive() {
    if (!mic_exclusive_) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr && codec->input_enabled()) {
            codec->EnableInput(false);
        }
        return;
    }
    mic_exclusive_ = false;
    saved_wake_word_ = false;
    saved_voice_proc_ = false;
    // Drop the mic path only. Do NOT RestoreAudioRouting here: that rebuilds Opus
    // and re-arms wake-word, which fights Radio re-enter (heap thrash + duplex RX
    // left on). Chat OnEnter calls RestoreAudioRouting when TTS/voice is needed.
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableInput(false);
    }
    ESP_LOGI(TAG, "mic exclusive released (input off, routing deferred to Chat) heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void CursorPage::CaptureMic() {
    if (!active_ || !mic_exclusive_) {
        return;
    }
    capturing_.store(true);
    if (!active_ || !mic_exclusive_) {
        capturing_.store(false);
        return;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        capturing_.store(false);
        return;
    }
    // Keep the power-save timer from closing RX while this page owns the mic.
    // Music Tick does not go through AudioService::ReadAudioData, so without
    // this the 15s idle gate EnableInput(false)s and the next tick re-opens
    // the duplex device — fragmenting no-PSRAM heap before Radio.
    Application::GetInstance().GetAudioService().NotifyInputActivity();
    if (!codec->input_enabled()) {
        codec->EnableInput(true);
    }

    if (mic_buf_.size() != static_cast<size_t>(kMicSamples)) {
        mic_buf_.assign(kMicSamples, 0);
    }
    if (!codec->InputData(mic_buf_)) {
        capturing_.store(false);
        return;
    }

    // Band energy: split PCM into kBarCount segments (time-slice spectrum proxy).
    const int n = static_cast<int>(mic_buf_.size());
    const int seg = n / kBarCount;
    if (seg <= 0) {
        capturing_.store(false);
        return;
    }

    float max_e = 0.001f;
    float energies[kBarCount];
    for (int b = 0; b < kBarCount; ++b) {
        double sum = 0;
        double diff_sum = 0;
        int16_t prev = 0;
        const int start = b * seg;
        const int end = (b == kBarCount - 1) ? n : (start + seg);
        for (int i = start; i < end; ++i) {
            int16_t s = mic_buf_[i];
            sum += static_cast<double>(s) * static_cast<double>(s);
            int d = static_cast<int>(s) - static_cast<int>(prev);
            diff_sum += static_cast<double>(d) * static_cast<double>(d);
            prev = s;
        }
        int count = end - start;
        float rms = static_cast<float>(std::sqrt(sum / count));
        float hif = static_cast<float>(std::sqrt(diff_sum / count));
        float w = 0.55f + 0.45f * (static_cast<float>(b) / (kBarCount - 1));
        energies[b] = rms * (1.0f - w) + hif * w;
        if (energies[b] > max_e) {
            max_e = energies[b];
        }
    }

    if (max_e > peak_) {
        peak_ = max_e;
    } else {
        peak_ = peak_ * 0.92f + max_e * 0.08f;
    }
    if (peak_ < 80.0f) {
        peak_ = 80.0f;
    }

    for (int b = 0; b < kBarCount; ++b) {
        float target = energies[b] / peak_;
        if (target > 1.0f) {
            target = 1.0f;
        }
        if (target > levels_[b]) {
            levels_[b] = levels_[b] * 0.35f + target * 0.65f;
        } else {
            levels_[b] = levels_[b] * 0.75f + target * 0.25f;
        }
    }
    capturing_.store(false);
}

void CursorPage::UpdateBars(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    const int plot_w = kBarCount * (kBarW + kBarGap) - kBarGap;
    const int plot_x = (kScreenW - plot_w) / 2;

    for (int i = 0; i < kBarCount; ++i) {
        if (bars_[i] == nullptr) {
            continue;
        }
        int h = static_cast<int>(levels_[i] * (kPlotH - 2));
        if (h < 2) {
            h = 2;
        }
        if (h > kPlotH) {
            h = kPlotH;
        }
        int x = plot_x + i * (kBarW + kBarGap);
        lv_obj_set_size(bars_[i], kBarW, h);
        lv_obj_set_pos(bars_[i], x, kPlotTop + kPlotH - h);

        uint32_t color = (i == 19) ? 0x22C55E : 0x00FFFF;
        if (levels_[i] > 0.85f && (i % 11 == 3)) {
            color = 0x22C55E;
        }
        lv_obj_set_style_bg_color(bars_[i], lv_color_hex(color), 0);
    }
}

void CursorPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    peak_ = 200.0f;
    ESP_LOGI(TAG, "OnEnter Music mic visualizer reuse=%d heap=%u largest=%u",
             panel_ != nullptr ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Always (re)take exclusive mic; OnLeave must restore even on quick switch.
    ReleaseMicExclusive();
    auto& audio = Application::GetInstance().GetAudioService();
    saved_wake_word_ = audio.IsWakeWordRunning();
    saved_voice_proc_ = audio.IsAudioProcessorRunning();
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(false);
    mic_exclusive_ = true;
    audio.NotifyInputActivity();

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->EnableInput(true);
    }

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed — releasing mic");
        ReleaseMicExclusive();
        FreeMicBuffer();
        active_ = false;
        return;
    }
    {
        DisplayLockGuard lock(display);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
    }
    display->HideChatUi();
}

void CursorPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    // Stop new ticks first. CaptureMic runs on the shared esp_timer task
    // (blocking I2S read); wait it out before closing RX so we do not close
    // esp_codec_dev under an in-flight read (leaks DMA / fragments heap).
    active_ = false;
    for (int i = 0; i < 40 && capturing_.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (capturing_.load()) {
        ESP_LOGW(TAG, "OnLeave: mic capture still in flight after wait");
    }
    ReleaseMicExclusive();
    FreeMicBuffer();
    ESP_LOGI(TAG, "OnLeave Music after mic release heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Drop the visualizer (24 bars + grid). Hidden panels stay resident and
    // Music→Launcher→Radio stacks three exclusive UIs on no-PSRAM; Radio's
    // MP3 decoder needs the contiguous block more than a cached Music page.
    DestroyPanel(display);
    ESP_LOGI(TAG, "OnLeave Music after panel destroy heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void CursorPage::Tick(CardputerAdvCarLcdDisplay* display) {
    if (!active_) {
        return;
    }
    CaptureMic();
    UpdateBars(display);
}
