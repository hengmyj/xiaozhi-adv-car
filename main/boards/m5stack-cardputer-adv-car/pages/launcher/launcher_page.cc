#include "launcher_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <ctime>

#define TAG "LauncherPage"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr lv_coord_t kStatusH = 24;
constexpr lv_coord_t kBtnW = 112;
constexpr lv_coord_t kBtnH = 24;
constexpr lv_coord_t kGapX = 6;
constexpr lv_coord_t kGapY = 2;
constexpr lv_coord_t kGridX = 5;
constexpr lv_coord_t kGridY = 26;

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

// Hollow dotted capital M ù sized to sit on the same visual baseline as "YJ"
// (BUILTIN_TEXT_FONT ~20px). Pitch 2 ? 14ù18px glyph.
constexpr int kMCols = 7;
constexpr int kMRows = 9;
constexpr const char* kMDots[kMRows] = {
    "*.....*",
    "**...**",
    "*.*.*.*",
    "*..*..*",
    "*.....*",
    "*.....*",
    "*.....*",
    "*.....*",
    "*.....*",
};

}  // namespace

void LauncherPage::BuildMyjLogo(lv_obj_t* parent) {
    lv_obj_t* logo = lv_obj_create(parent);
    StripStyles(logo);
    lv_obj_set_size(logo, 72, kStatusH);
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 2, 0);

    constexpr int kDot = 2;
    constexpr int kPitch = 2;
    const int m_w = kMCols * kPitch;
    const int m_h = kMRows * kPitch;

    lv_obj_t* m_box = lv_obj_create(logo);
    StripStyles(m_box);
    lv_obj_set_size(m_box, m_w, m_h);
    lv_obj_align(m_box, LV_ALIGN_LEFT_MID, 0, 0);

    static const uint32_t kDotColors[] = {0xFF5555, 0x55FF55, 0x5599FF, 0xFFCC33};
    for (int r = 0; r < kMRows; ++r) {
        const char* row = kMDots[r];
        for (int c = 0; c < kMCols; ++c) {
            if (row[c] != '*') {
                continue;
            }
            lv_obj_t* dot = lv_obj_create(m_box);
            StripStyles(dot);
            lv_obj_set_size(dot, kDot, kDot);
            lv_obj_set_pos(dot, c * kPitch, r * kPitch);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColors[(r + c) % 4]), 0);
        }
    }

    lv_obj_t* yj = lv_label_create(logo);
    lv_label_set_text(yj, "YJ");
    lv_obj_set_style_text_font(yj, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(yj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_letter_space(yj, 1, 0);
    lv_obj_align(yj, LV_ALIGN_LEFT_MID, m_w + 4, 0);
}

lv_obj_t* LauncherPage::MakeAppButton(lv_obj_t* parent, const char* badge, const char* title,
                                      uint32_t color, int x, int y, int w, int h) {
    lv_obj_t* btn = lv_obj_create(parent);
    StripStyles(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* badge_box = lv_obj_create(btn);
    StripStyles(badge_box);
    lv_obj_set_size(badge_box, 16, 16);
    lv_obj_set_pos(badge_box, 5, (h - 16) / 2);
    lv_obj_set_style_bg_color(badge_box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(badge_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge_box, 3, 0);

    lv_obj_t* badge_lbl = lv_label_create(badge_box);
    lv_label_set_text(badge_lbl, badge);
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(badge_lbl);

    lv_obj_t* title_lbl = lv_label_create(btn);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 26, 0);

    return btn;
}

void LauncherPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    time_label_ = nullptr;
    for (int i = 0; i < kAppCount; ++i) {
        btns_[i] = nullptr;
    }
}

void LauncherPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
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

    lv_obj_t* status = lv_obj_create(panel_);
    StripStyles(status);
    lv_obj_set_size(status, kScreenW, kStatusH);
    lv_obj_set_pos(status, 0, 0);

    BuildMyjLogo(status);

    time_label_ = lv_label_create(status);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(0x00FF66), 0);
    lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* wifi = lv_label_create(status);
    lv_label_set_text(wifi, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(wifi, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(wifi, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(wifi, LV_ALIGN_RIGHT_MID, -6, 0);

    // ASCII titles avoid garbled CJK when glyph subset is incomplete.
    const int col2 = kGridX + kBtnW + kGapX;
    const int row2 = kGridY + kBtnH + kGapY;
    const int row3 = row2 + kBtnH + kGapY;
    const int row4 = row3 + kBtnH + kGapY;

    btns_[0] = MakeAppButton(panel_, "1", "Car", 0xF0C040, kGridX, kGridY, kBtnW, kBtnH);
    btns_[1] = MakeAppButton(panel_, "2", "SpiderBot", 0xA855F7, col2, kGridY, kBtnW, kBtnH);
    btns_[2] = MakeAppButton(panel_, "3", "IceBox", 0x3B82F6, kGridX, row2, kBtnW, kBtnH);
    btns_[3] = MakeAppButton(panel_, "4", "Clock", 0x22C55E, col2, row2, kBtnW, kBtnH);
    btns_[4] = MakeAppButton(panel_, "5", "Rain", 0x10B981, kGridX, row3, kBtnW, kBtnH);
    btns_[5] = MakeAppButton(panel_, "6", "Music", 0x06B6D4, col2, row3, kBtnW, kBtnH);
    btns_[6] = MakeAppButton(panel_, "7", "Radio", 0x00FF66, kGridX, row4, kBtnW, kBtnH);
}

void LauncherPage::RefreshTime(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || time_label_ == nullptr || display == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_sec == last_sec_) {
        return;
    }
    last_sec_ = tm_now.tm_sec;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    DisplayLockGuard lock(display);
    lv_label_set_text(time_label_, buf);
}

void LauncherPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    ESP_LOGI(TAG, "OnEnter launcher (MYJ) reuse=%d heap=%u largest=%u", panel_ != nullptr ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

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
    ESP_LOGI(TAG, "OnEnter launcher done heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void LauncherPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    if (display == nullptr || panel_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "OnLeave launcher hidden heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void LauncherPage::Tick(CardputerAdvCarLcdDisplay* display) {
    RefreshTime(display);
}

bool LauncherPage::HandleKey(const KeyEvent& event) {
    if (!event.pressed || event.is_modifier || !active_) {
        return false;
    }

    PageId target = PageId::Chat;
    bool matched = false;
    char ch = (event.key_char && event.key_char[0]) ? event.key_char[0] : '\0';

    if (event.key_code == KC_1 || ch == '1') {
        target = PageId::Car;
        matched = true;
    } else if (event.key_code == KC_2 || ch == '2') {
        target = PageId::Spider;
        matched = true;
    } else if (event.key_code == KC_3 || ch == '3') {
        target = PageId::MjAc;
        matched = true;
    } else if (event.key_code == KC_4 || ch == '4') {
        target = PageId::Clock;
        matched = true;
    } else if (event.key_code == KC_5 || ch == '5') {
        target = PageId::Matrix;
        matched = true;
    } else if (event.key_code == KC_6 || ch == '6') {
        target = PageId::Music;
        matched = true;
    } else if (event.key_code == KC_7 || ch == '7') {
        target = PageId::Radio;
        matched = true;
    }

    if (!matched || !navigate_) {
        return false;
    }

    ESP_LOGI(TAG, "launcher -> page %d", static_cast<int>(target));
    navigate_(target);
    return true;
}
