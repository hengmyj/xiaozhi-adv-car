#include "clock_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>
#include <ctime>
#include <cstring>

#define TAG "ClockPage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;

// Pixel glyph (same shapes as before) drawn into ONE canvas — avoids creating
// hundreds of lv_obj dots that exhausted internal heap / held DisplayLock too long.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kTimePx = 4;
constexpr int kTimeGap = 3;
constexpr int kDatePx = 2;
constexpr int kDateGap = 2;
constexpr int kTimeChars = 8;   // HH:MM:SS
constexpr int kDateChars = 10;  // YYYY-MM-DD

constexpr int kTimeCell = kGlyphW * kTimePx;
constexpr int kDateCell = kGlyphW * kDatePx;
// Pad width even so RGB565 row bytes (w*2) stay 4-byte aligned for LVGL canvas.
constexpr int kCanvasWRaw = kTimeChars * kTimeCell + (kTimeChars - 1) * kTimeGap;  // 181
constexpr int kCanvasW = (kCanvasWRaw + 1) & ~1;                                  // 182
constexpr int kCanvasH = kGlyphH * kTimePx + 12 + kGlyphH * kDatePx;              // 54

static_assert(kCanvasW == 182, "clock canvas width mismatch vs header");
static_assert(kCanvasH == 54, "clock canvas height mismatch vs header");

constexpr const char* kDigitGlyphs[11][kGlyphH] = {
    {"*****", "*...*", "*...*", "*...*", "*...*", "*...*", "*****"},
    {"..*..", ".**..", "..*..", "..*..", "..*..", "..*..", "*****"},
    {"*****", "....*", "....*", "*****", "*....", "*....", "*****"},
    {"*****", "....*", "....*", "*****", "....*", "....*", "*****"},
    {"*...*", "*...*", "*...*", "*****", "....*", "....*", "....*"},
    {"*****", "*....", "*....", "*****", "....*", "....*", "*****"},
    {"*****", "*....", "*....", "*****", "*...*", "*...*", "*****"},
    {"*****", "....*", "....*", "...*.", "..*..", "..*..", "..*.."},
    {"*****", "*...*", "*...*", "*****", "*...*", "*...*", "*****"},
    {"*****", "*...*", "*...*", "*****", "....*", "....*", "*****"},
    {".....", "..*..", "..*..", ".....", "..*..", "..*..", "....."},
};

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

int GlyphIndex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c == ':') {
        return 10;
    }
    return -1;
}

void FillRect(uint16_t* buf, int stride, int x0, int y0, int w, int h, uint16_t color) {
    for (int y = y0; y < y0 + h; ++y) {
        if (y < 0 || y >= kCanvasH) {
            continue;
        }
        uint16_t* row = buf + y * stride;
        for (int x = x0; x < x0 + w; ++x) {
            if (x >= 0 && x < kCanvasW) {
                row[x] = color;
            }
        }
    }
}

void PaintGlyphInto(uint16_t* buf, char c, int origin_x, int origin_y, int px) {
    constexpr uint16_t kWhite = 0xFFFF;  // RGB565 white
    if (c == '-') {
        FillRect(buf, kCanvasW, origin_x + px, origin_y + 3 * px, 3 * px, px, kWhite);
        return;
    }
    int gi = GlyphIndex(c);
    if (gi < 0) {
        return;
    }
    for (int r = 0; r < kGlyphH; ++r) {
        for (int col = 0; col < kGlyphW; ++col) {
            if (kDigitGlyphs[gi][r][col] != '*') {
                continue;
            }
            FillRect(buf, kCanvasW, origin_x + col * px, origin_y + r * px, px, px, kWhite);
        }
    }
}

}  // namespace

void ClockPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    canvas_ = nullptr;
    last_sec_ = -1;
    last_drawn_[0] = '\0';
    ESP_LOGI(TAG, "DestroyPanel clock heap %u->%u largest %u->%u (canvas BSS %u stays)",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)sizeof(canvas_buf_));
}

void ClockPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ != nullptr) {
        return;
    }

    ESP_LOGI(TAG, "BuildPanel canvas %dx%d buf=%u heap=%u largest=%u", kCanvasW, kCanvasH,
             static_cast<unsigned>(sizeof(canvas_buf_)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    StripStyles(panel_);
    lv_obj_set_size(panel_, kScreenW, kScreenH);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);

    canvas_ = lv_canvas_create(panel_);
    lv_canvas_set_buffer(canvas_, canvas_buf_, kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas_, LV_ALIGN_TOP_MID, 0, 36);
    std::memset(canvas_buf_, 0, sizeof(canvas_buf_));
    lv_obj_invalidate(canvas_);
}

void ClockPage::DrawClock(const char* time_buf, const char* date_buf) {
    if (canvas_ == nullptr) {
        return;
    }
    // Skip redraw if identical string already painted (Tick may fire 10x/s).
    char key[48];
    std::snprintf(key, sizeof(key), "%s %s", time_buf, date_buf);
    if (std::strcmp(key, last_drawn_) == 0) {
        return;
    }
    std::strncpy(last_drawn_, key, sizeof(last_drawn_) - 1);
    last_drawn_[sizeof(last_drawn_) - 1] = '\0';

    std::memset(canvas_buf_, 0, sizeof(canvas_buf_));
    for (int i = 0; i < kTimeChars; ++i) {
        int x = i * (kTimeCell + kTimeGap);
        PaintGlyphInto(canvas_buf_, time_buf[i], x, 0, kTimePx);
    }
    const int date_y = kGlyphH * kTimePx + 12;
    for (int i = 0; i < kDateChars; ++i) {
        int x = i * (kDateCell + kDateGap);
        // Center date under time block.
        const int date_w = kDateChars * kDateCell + (kDateChars - 1) * kDateGap;
        const int x_off = (kCanvasW - date_w) / 2;
        PaintGlyphInto(canvas_buf_, date_buf[i], x_off + x, date_y, kDatePx);
    }
    lv_obj_invalidate(canvas_);
}

void ClockPage::RefreshTime(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr || refreshing_) {
        return;
    }
    refreshing_ = true;

    const int64_t t0 = esp_timer_get_time();
    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);

    if (tm_now.tm_sec == last_sec_) {
        refreshing_ = false;
        return;
    }
    last_sec_ = tm_now.tm_sec;

    char time_buf[16];
    char date_buf[40];
    std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min,
                  tm_now.tm_sec);
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", tm_now.tm_year + 1900,
                  tm_now.tm_mon + 1, tm_now.tm_mday);

    // Short lock: buffer paint is CPU-only; only invalidate under LVGL lock.
    {
        DisplayLockGuard lock(display);
        DrawClock(time_buf, date_buf);
    }

    const int64_t dt_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "tick ok %s heap=%u dt_us=%lld", time_buf,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<long long>(dt_us));
    refreshing_ = false;
}

void ClockPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    refreshing_ = false;
    ESP_LOGI(TAG, "OnEnter clock reuse=%d heap=%u largest=%u", panel_ != nullptr ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        active_ = false;
        return;
    }
    {
        DisplayLockGuard lock(display);
        last_drawn_[0] = '\0';
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
    }
    // HideChatUi takes its own lock — never nest.
    display->HideChatUi();
    last_sec_ = -1;
    RefreshTime(display);
    ESP_LOGI(TAG, "OnEnter done heap=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void ClockPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    ESP_LOGI(TAG, "OnLeave clock heap=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    active_ = false;
    refreshing_ = false;
    DestroyPanel(display);
}

void ClockPage::ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) {
    DestroyPanel(display);
}

void ClockPage::Tick(CardputerAdvCarLcdDisplay* display) {
    // Must stay short / non-blocking: runs on esp_timer task.
    RefreshTime(display);
}
