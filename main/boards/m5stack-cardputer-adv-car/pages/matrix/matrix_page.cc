#include "matrix_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#define TAG "MatrixPage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr int kCharH = 12;
constexpr int kColW = 24;
constexpr uint32_t kHeapLogEvery = 50;  // ~5s at 100ms tick

const char kCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%&*<>/\\|";

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

uint8_t BandFromLife(uint8_t life, bool is_head) {
    if (life == 0) {
        return 0;  // kBandDead
    }
    if (is_head && life > 200) {
        return 4;  // kBandHead
    }
    if (life > 160) {
        return 3;  // kBandBright
    }
    if (life > 80) {
        return 2;  // kBandMid
    }
    return 1;  // kBandDim
}

}  // namespace

void MatrixPage::ApplyCellVisual(int c, int r, uint8_t band) {
    lv_obj_t* cell = cells_[c][r];
    if (cell == nullptr) {
        return;
    }
    band_[c][r] = band;
    switch (band) {
        case kBandHead:
            lv_obj_set_style_text_color(cell, lv_color_hex(0xCCFFCC), 0);
            lv_obj_set_style_text_opa(cell, 230, 0);
            break;
        case kBandBright:
            lv_obj_set_style_text_color(cell, lv_color_hex(0x33FF88), 0);
            lv_obj_set_style_text_opa(cell, 180, 0);
            break;
        case kBandMid:
            lv_obj_set_style_text_color(cell, lv_color_hex(0x009944), 0);
            lv_obj_set_style_text_opa(cell, 120, 0);
            break;
        case kBandDim:
            lv_obj_set_style_text_color(cell, lv_color_hex(0x006622), 0);
            lv_obj_set_style_text_opa(cell, 70, 0);
            break;
        default:
            text_buf_[c][r][0] = ' ';
            text_buf_[c][r][1] = '\0';
            lv_label_set_text_static(cell, text_buf_[c][r]);
            lv_obj_set_style_text_opa(cell, LV_OPA_TRANSP, 0);
            break;
    }
}

void MatrixPage::ResetAnimationState() {
    tick_count_ = 0;
    for (int c = 0; c < kCols; ++c) {
        last_row_[c] = -999;
        for (int r = 0; r < kRows; ++r) {
            life_[c][r] = 0;
            band_[c][r] = kBandDead;
            text_buf_[c][r][0] = ' ';
            text_buf_[c][r][1] = '\0';
        }
    }
}

void MatrixPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    for (int c = 0; c < kCols; ++c) {
        for (int r = 0; r < kRows; ++r) {
            cells_[c][r] = nullptr;
            life_[c][r] = 0;
            band_[c][r] = kBandDead;
            text_buf_[c][r][0] = ' ';
            text_buf_[c][r][1] = '\0';
        }
    }
}

void MatrixPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
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

    rng_ = 0xA5A5u ^ static_cast<uint32_t>(esp_log_timestamp());
    for (int c = 0; c < kCols; ++c) {
        rng_ = rng_ * 1664525u + 1013904223u;
        head_y_[c] = static_cast<float>(static_cast<int>(rng_ % 140) - 100);
        speed_[c] = 0.9f + static_cast<float>(rng_ % 50) / 40.0f;
        last_row_[c] = -999;

        for (int r = 0; r < kRows; ++r) {
            cells_[c][r] = lv_label_create(panel_);
            text_buf_[c][r][0] = ' ';
            text_buf_[c][r][1] = '\0';
            lv_obj_set_style_text_font(cells_[c][r], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(cells_[c][r], lv_color_hex(0x00FF66), 0);
            lv_obj_set_style_text_opa(cells_[c][r], LV_OPA_TRANSP, 0);
            // Static text: no free/malloc on glyph change (was heap-fragmenting).
            lv_label_set_text_static(cells_[c][r], text_buf_[c][r]);
            lv_obj_set_pos(cells_[c][r], c * kColW + 2, r * kCharH);
            life_[c][r] = 0;
            band_[c][r] = kBandDead;
        }
    }
}

void MatrixPage::StepAnimation(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr) {
        return;
    }

    ++tick_count_;
    if ((tick_count_ % kHeapLogEvery) == 0) {
        const uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const int32_t delta = static_cast<int32_t>(heap) - static_cast<int32_t>(heap_at_enter_);
        ESP_LOGI(TAG, "tick=%u heap=%u delta=%ld", static_cast<unsigned>(tick_count_),
                 static_cast<unsigned>(heap), static_cast<long>(delta));
        // Soft assert: matrix must not steadily consume internal heap.
        if (delta < -4096) {
            ESP_LOGW(TAG, "heap dropped >4KB since enter (possible leak)");
        }
    }

    DisplayLockGuard lock(display);
    // Cap work: only touch cells whose glyph/band actually changed.
    for (int c = 0; c < kCols; ++c) {
        head_y_[c] += speed_[c];
        if (head_y_[c] > kScreenH + kTrail * kCharH) {
            rng_ = rng_ * 1664525u + 1013904223u;
            head_y_[c] = static_cast<float>(-static_cast<int>(rng_ % 60) - 20);
            speed_[c] = 0.8f + static_cast<float>(rng_ % 55) / 40.0f;
            last_row_[c] = -999;
        }

        const int head_row = static_cast<int>(head_y_[c] / kCharH);
        if (head_row != last_row_[c] && head_row >= 0 && head_row < kRows) {
            last_row_[c] = head_row;
            rng_ = rng_ * 1664525u + 1013904223u;
            text_buf_[c][head_row][0] = kCharset[rng_ % (sizeof(kCharset) - 1)];
            text_buf_[c][head_row][1] = '\0';
            life_[c][head_row] = 255;
            lv_label_set_text_static(cells_[c][head_row], text_buf_[c][head_row]);
        }

        for (int r = 0; r < kRows; ++r) {
            if (life_[c][r] == 0) {
                // Already dead: do not touch LVGL every tick (was 110 style ops/frame).
                continue;
            }

            const int decay = 6 + (r + c) % 4;
            if (life_[c][r] > decay) {
                life_[c][r] = static_cast<uint8_t>(life_[c][r] - decay);
            } else {
                life_[c][r] = 0;
            }

            if (life_[c][r] == 0) {
                ApplyCellVisual(c, r, kBandDead);
                continue;
            }

            // Occasional glyph morph in the trail (static buf, no malloc).
            if (life_[c][r] > 40 && ((rng_ + c * 17 + r) % 11 == 0)) {
                rng_ = rng_ * 1664525u + 1013904223u;
                text_buf_[c][r][0] = kCharset[rng_ % (sizeof(kCharset) - 1)];
                text_buf_[c][r][1] = '\0';
                lv_label_set_text_static(cells_[c][r], text_buf_[c][r]);
            }

            const uint8_t band = BandFromLife(life_[c][r], r == head_row);
            if (band != band_[c][r]) {
                ApplyCellVisual(c, r, band);
            }
        }
    }
}

void MatrixPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    heap_at_enter_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "OnEnter matrix rain reuse=%d heap=%u", panel_ != nullptr ? 1 : 0,
             static_cast<unsigned>(heap_at_enter_));

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        return;
    }

    // Re-seed drop heads; clear trails so reused panel has no stale life.
    {
        DisplayLockGuard lock(display);
        ResetAnimationState();
        rng_ = 0xA5A5u ^ static_cast<uint32_t>(esp_log_timestamp());
        for (int c = 0; c < kCols; ++c) {
            rng_ = rng_ * 1664525u + 1013904223u;
            head_y_[c] = static_cast<float>(static_cast<int>(rng_ % 140) - 100);
            speed_[c] = 0.9f + static_cast<float>(rng_ % 50) / 40.0f;
            for (int r = 0; r < kRows; ++r) {
                if (cells_[c][r] != nullptr) {
                    ApplyCellVisual(c, r, kBandDead);
                }
            }
        }
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
    }
    display->HideChatUi();
    StepAnimation(display);
}

void MatrixPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    if (display == nullptr || panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    // Stop animation state so Tick does no LVGL work after leave.
    ResetAnimationState();
    for (int c = 0; c < kCols; ++c) {
        for (int r = 0; r < kRows; ++r) {
            if (cells_[c][r] != nullptr) {
                ApplyCellVisual(c, r, kBandDead);
            }
        }
    }
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void MatrixPage::Tick(CardputerAdvCarLcdDisplay* display) {
    StepAnimation(display);
}
