#include "clock_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_log.h>

#include <cstdio>
#include <ctime>
#include <cstring>

#define TAG "ClockPage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kTimePx = 4;
constexpr int kTimeGap = 3;
constexpr int kDatePx = 2;
constexpr int kDateGap = 2;
constexpr int kTimeChars = 8;   // HH:MM:SS
constexpr int kDateChars = 10;  // YYYY-MM-DD
constexpr int kMaxDots = kGlyphW * kGlyphH;

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

struct GlyphSlot {
    lv_obj_t* dots[kMaxDots] = {};
    int dot_count = 0;
    char shown = '\0';
};

GlyphSlot g_time_slots[kTimeChars];
GlyphSlot g_date_slots[kDateChars];

void ClearSlot(GlyphSlot* slot) {
    for (int i = 0; i < slot->dot_count; ++i) {
        if (slot->dots[i] != nullptr) {
            lv_obj_add_flag(slot->dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void EnsureDots(lv_obj_t* parent, GlyphSlot* slot, int need) {
    while (slot->dot_count < need && slot->dot_count < kMaxDots) {
        lv_obj_t* dot = lv_obj_create(parent);
        StripStyles(dot);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        slot->dots[slot->dot_count++] = dot;
    }
}

void PaintGlyph(lv_obj_t* parent, GlyphSlot* slot, char c, int origin_x, int origin_y, int px) {
    if (slot->shown == c) {
        return;
    }
    slot->shown = c;
    ClearSlot(slot);

    if (c == '-') {
        EnsureDots(parent, slot, 3);
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* dot = slot->dots[i];
            lv_obj_set_size(dot, px, px);
            lv_obj_set_pos(dot, origin_x + (i + 1) * px, origin_y + 3 * px);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    int gi = GlyphIndex(c);
    if (gi < 0) {
        return;
    }

    int need = 0;
    for (int r = 0; r < kGlyphH; ++r) {
        for (int col = 0; col < kGlyphW; ++col) {
            if (kDigitGlyphs[gi][r][col] == '*') {
                ++need;
            }
        }
    }
    EnsureDots(parent, slot, need);

    int di = 0;
    for (int r = 0; r < kGlyphH; ++r) {
        for (int col = 0; col < kGlyphW; ++col) {
            if (kDigitGlyphs[gi][r][col] != '*') {
                continue;
            }
            lv_obj_t* dot = slot->dots[di++];
            lv_obj_set_size(dot, px, px);
            lv_obj_set_pos(dot, origin_x + col * px, origin_y + r * px);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ResetSlots() {
    for (int i = 0; i < kTimeChars; ++i) {
        g_time_slots[i] = {};
    }
    for (int i = 0; i < kDateChars; ++i) {
        g_date_slots[i] = {};
    }
}

}  // namespace

void ClockPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    time_label_ = nullptr;
    date_label_ = nullptr;
    last_sec_ = -1;
    ResetSlots();
}

void ClockPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ != nullptr) {
        return;
    }

    ResetSlots();
    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    StripStyles(panel_);
    lv_obj_set_size(panel_, kScreenW, kScreenH);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);

    const int time_cell = kGlyphW * kTimePx;
    const int time_w = kTimeChars * time_cell + (kTimeChars - 1) * kTimeGap;
    const int time_h = kGlyphH * kTimePx;
    time_label_ = lv_obj_create(panel_);
    StripStyles(time_label_);
    lv_obj_set_size(time_label_, time_w, time_h);
    lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 36);

    const int date_cell = kGlyphW * kDatePx;
    const int date_w = kDateChars * date_cell + (kDateChars - 1) * kDateGap;
    const int date_h = kGlyphH * kDatePx;
    date_label_ = lv_obj_create(panel_);
    StripStyles(date_label_);
    lv_obj_set_size(date_label_, date_w, date_h);
    lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 36 + time_h + 12);
}

void ClockPage::RefreshTime(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);

    if (tm_now.tm_sec == last_sec_) {
        return;
    }
    last_sec_ = tm_now.tm_sec;

    char time_buf[32];
    char date_buf[32];
    std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min,
                  tm_now.tm_sec);
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d", tm_now.tm_year + 1900,
                  tm_now.tm_mon + 1, tm_now.tm_mday);

    DisplayLockGuard lock(display);
    const int time_cell = kGlyphW * kTimePx;
    for (int i = 0; i < kTimeChars; ++i) {
        int x = i * (time_cell + kTimeGap);
        PaintGlyph(time_label_, &g_time_slots[i], time_buf[i], x, 0, kTimePx);
    }
    const int date_cell = kGlyphW * kDatePx;
    for (int i = 0; i < kDateChars; ++i) {
        int x = i * (date_cell + kDateGap);
        PaintGlyph(date_label_, &g_date_slots[i], date_buf[i], x, 0, kDatePx);
    }
}

void ClockPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    ESP_LOGI(TAG, "OnEnter clock reuse=%d", panel_ != nullptr ? 1 : 0);

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        return;
    }
    {
        DisplayLockGuard lock(display);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
    }
    display->HideChatUi();
    last_sec_ = -1;
    RefreshTime(display);
}

void ClockPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    if (display == nullptr || panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void ClockPage::Tick(CardputerAdvCarLcdDisplay* display) {
    RefreshTime(display);
}
