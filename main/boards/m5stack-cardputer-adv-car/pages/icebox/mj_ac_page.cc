#include "mj_ac_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <font_awesome.h>

#include <esp_log.h>
#include <esp_timer.h>

#include <cctype>
#include <cstdio>

#define TAG "MjAcPage"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {

// 240x135 Sparks-style layout
constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr lv_coord_t kLeftW = 92;
constexpr lv_coord_t kRightX = 92;
constexpr lv_coord_t kIconRowH = 22;
constexpr lv_coord_t kBtnW = 46;
constexpr lv_coord_t kBtnH = 50;
constexpr lv_coord_t kBtnGapX = 3;
constexpr lv_coord_t kBtnGapY = 3;
constexpr lv_coord_t kGridX = 4;
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

lv_obj_t* MakeKeyButton(lv_obj_t* parent, const char* key, const char* caption, int x, int y,
                        bool key_is_icon) {
    lv_obj_t* btn = lv_obj_create(parent);
    StripStyles(btn);
    lv_obj_set_size(btn, kBtnW, kBtnH);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x9A9A9A), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* key_lbl = lv_label_create(btn);
    lv_label_set_text(key_lbl, key);
    lv_obj_set_style_text_color(key_lbl, lv_color_hex(0xFFFFFF), 0);
    if (key_is_icon) {
        lv_obj_set_style_text_font(key_lbl, &BUILTIN_ICON_FONT, 0);
    } else {
        lv_obj_set_style_text_font(key_lbl, &lv_font_montserrat_14, 0);
    }
    lv_obj_align(key_lbl, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t* cap = lv_label_create(btn);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(cap, -1, 0);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -4);
    return btn;
}

}  // namespace

char MjAcPage::KeyCharLower(const KeyEvent& event) {
    if (event.key_char == nullptr || event.key_char[0] == '\0') {
        return '\0';
    }
    return static_cast<char>(std::tolower(static_cast<unsigned char>(event.key_char[0])));
}

void MjAcPage::FlashButton(int index) {
    if (index < 0 || index >= 6) {
        return;
    }
    btn_flash_index_ = index;
    btn_flash_until_us_ = esp_timer_get_time() + 220000;
    ui_dirty_ = true;
}

void MjAcPage::ClearButtonFlash() {
    for (int i = 0; i < 6; ++i) {
        if (btns_[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(btns_[i], lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_border_color(btns_[i], lv_color_hex(0x9A9A9A), 0);
    }
}

void MjAcPage::UpdateModeIcons() {
    if (mode_cool_ == nullptr) {
        return;
    }

    const bool on = state_.power;
    lv_obj_set_style_text_color(
        mode_cool_,
        (on && state_.mode == MjAcMode::Cool) ? lv_color_hex(0x4DB8FF) : lv_color_hex(0x555555),
        0);
    lv_obj_set_style_text_color(
        mode_fan_,
        (on && (state_.mode == MjAcMode::Fan || state_.fan != MjAcFan::Auto))
            ? lv_color_hex(0xFFFFFF)
            : lv_color_hex(0x777777),
        0);
    if (fan_bar_ != nullptr) {
        lv_obj_set_style_bg_color(
            fan_bar_,
            (on && state_.fan != MjAcFan::Auto) ? lv_color_hex(0x22C55E) : lv_color_hex(0x333333),
            0);
    }
    // Power icon: bright green when ON, dim red-gray when OFF ù obvious toggle feedback.
    lv_obj_set_style_text_color(
        mode_power_,
        on ? lv_color_hex(0x22C55E) : lv_color_hex(0xFF5555),
        0);
}

void MjAcPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    temp_label_ = nullptr;
    logo_label_ = nullptr;
    mode_cool_ = nullptr;
    mode_fan_ = nullptr;
    mode_power_ = nullptr;
    fan_bar_ = nullptr;
    tx_dot_ = nullptr;
    status_label_ = nullptr;
    for (int i = 0; i < 6; ++i) {
        btns_[i] = nullptr;
    }
}

void MjAcPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
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

    lv_obj_t* left = lv_obj_create(panel_);
    StripStyles(left);
    lv_obj_set_size(left, kLeftW, kScreenH);
    lv_obj_set_pos(left, 0, 0);

    lv_obj_t* wifi = lv_label_create(left);
    lv_label_set_text(wifi, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(wifi, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(wifi, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(wifi, 8, 4);

    temp_label_ = lv_label_create(left);
    lv_label_set_text(temp_label_, "26\xc2\xb0""C");
    lv_obj_set_style_text_font(temp_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(temp_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_letter_space(temp_label_, 1, 0);
    lv_obj_align(temp_label_, LV_ALIGN_LEFT_MID, 10, -6);

    logo_label_ = lv_label_create(left);
    lv_label_set_text(logo_label_, "MJ");
    lv_obj_set_style_text_font(logo_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(logo_label_, lv_color_hex(0xFF7A00), 0);
    lv_obj_set_style_text_letter_space(logo_label_, 2, 0);
    lv_obj_align(logo_label_, LV_ALIGN_BOTTOM_LEFT, 14, -10);

    status_label_ = lv_label_create(left);
    lv_label_set_text(status_label_, "ON");
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x22C55E), 0);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_LEFT, 14, -28);

    tx_dot_ = lv_obj_create(left);
    StripStyles(tx_dot_);
    lv_obj_set_size(tx_dot_, 7, 7);
    lv_obj_set_style_radius(tx_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(tx_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tx_dot_, lv_color_hex(0x333333), 0);
    lv_obj_align(tx_dot_, LV_ALIGN_TOP_RIGHT, -6, 8);

    lv_obj_t* right = lv_obj_create(panel_);
    StripStyles(right);
    lv_obj_set_size(right, kScreenW - kLeftW, kScreenH);
    lv_obj_set_pos(right, kRightX, 0);

    lv_obj_t* icons = lv_obj_create(right);
    StripStyles(icons);
    lv_obj_set_size(icons, kScreenW - kLeftW - 4, kIconRowH);
    lv_obj_set_pos(icons, 2, 2);

    mode_cool_ = lv_label_create(icons);
    lv_label_set_text(mode_cool_, FONT_AWESOME_SNOWFLAKE);
    lv_obj_set_style_text_font(mode_cool_, &BUILTIN_ICON_FONT, 0);
    lv_obj_align(mode_cool_, LV_ALIGN_LEFT_MID, 18, 0);

    fan_bar_ = lv_obj_create(icons);
    StripStyles(fan_bar_);
    lv_obj_set_size(fan_bar_, 3, 12);
    lv_obj_set_style_radius(fan_bar_, 1, 0);
    lv_obj_set_style_bg_opa(fan_bar_, LV_OPA_COVER, 0);
    lv_obj_align(fan_bar_, LV_ALIGN_CENTER, -4, 0);

    mode_fan_ = lv_label_create(icons);
    lv_label_set_text(mode_fan_, FONT_AWESOME_WIND);
    lv_obj_set_style_text_font(mode_fan_, &BUILTIN_ICON_FONT, 0);
    lv_obj_align(mode_fan_, LV_ALIGN_CENTER, 10, 0);

    mode_power_ = lv_label_create(icons);
    lv_label_set_text(mode_power_, FONT_AWESOME_POWER_OFF);
    lv_obj_set_style_text_font(mode_power_, &BUILTIN_ICON_FONT, 0);
    lv_obj_align(mode_power_, LV_ALIGN_RIGHT_MID, -12, 0);

    const int step_x = kBtnW + kBtnGapX;
    const int step_y = kBtnH + kBtnGapY;

    btns_[0] = MakeKeyButton(right, "P", "Power", kGridX, kGridY, false);
    btns_[1] = MakeKeyButton(right, "F", "Fan", kGridX + step_x, kGridY, false);
    btns_[2] = MakeKeyButton(right, FONT_AWESOME_ARROW_UP, "Temp", kGridX + 2 * step_x, kGridY, true);
    btns_[3] = MakeKeyButton(right, "S", "Send", kGridX, kGridY + step_y, false);
    btns_[4] = MakeKeyButton(right, "M", "Mode", kGridX + step_x, kGridY + step_y, false);
    btns_[5] = MakeKeyButton(right, FONT_AWESOME_ARROW_DOWN, "Temp", kGridX + 2 * step_x,
                             kGridY + step_y, true);
}

void MjAcPage::RefreshUi(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);

    char buf[16];
    snprintf(buf, sizeof(buf), "%u\xc2\xb0""C", state_.temp_c);
    lv_label_set_text(temp_label_, buf);
    lv_obj_set_style_text_color(
        temp_label_,
        state_.power ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x666666),
        0);

    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, state_.power ? "ON" : "OFF");
        lv_obj_set_style_text_color(
            status_label_,
            state_.power ? lv_color_hex(0x22C55E) : lv_color_hex(0xFF5555),
            0);
    }

    UpdateModeIcons();

    const bool tx_active = esp_timer_get_time() < tx_active_until_us_;
    lv_obj_set_style_bg_color(tx_dot_, tx_active ? lv_color_hex(0xFF2D95) : lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(tx_dot_, tx_active ? LV_OPA_COVER : LV_OPA_40, 0);

    const int64_t now = esp_timer_get_time();
    const bool flash_on = (btn_flash_index_ >= 0 && now < btn_flash_until_us_);
    ClearButtonFlash();
    if (flash_on && btns_[btn_flash_index_] != nullptr) {
        lv_obj_set_style_bg_color(btns_[btn_flash_index_], lv_color_hex(0x4A4A4A), 0);
        lv_obj_set_style_border_color(btns_[btn_flash_index_], lv_color_hex(0xFFFFFF), 0);
        last_flash_index_ = btn_flash_index_;
    } else {
        btn_flash_index_ = -1;
        last_flash_index_ = -1;
    }

    last_power_ = state_.power;
    last_temp_ = state_.temp_c;
    last_mode_ = state_.mode;
    last_fan_ = state_.fan;
    last_tx_ = tx_active;
    ui_dirty_ = false;
}

void MjAcPage::MarkDirtyAndRefresh() {
    ui_dirty_ = true;
    if (display_ != nullptr && active_) {
        // Immediate UI feedback on the same key path ù do not wait for the 200 ms tick.
        RefreshUi(display_);
    }
}

void MjAcPage::ApplyAndSend() {
    // Non-blocking queue ? mj_ir_tx worker. Never block the keyboard task.
    if (ir_.Send(state_)) {
        tx_active_until_us_ = esp_timer_get_time() + 450000;
        ESP_LOGI(TAG, "IR auto-send queued power=%d temp=%u mode=%d fan=%d",
                 state_.power, state_.temp_c, static_cast<int>(state_.mode),
                 static_cast<int>(state_.fan));
    } else {
        ESP_LOGW(TAG, "IR auto-send queue failed");
    }
    MarkDirtyAndRefresh();
}

void MjAcPage::ForceResend() {
    if (ir_.Send(state_)) {
        tx_active_until_us_ = esp_timer_get_time() + 450000;
        ESP_LOGI(TAG, "IR force-resend queued power=%d temp=%u mode=%d fan=%d",
                 state_.power, state_.temp_c, static_cast<int>(state_.mode),
                 static_cast<int>(state_.fan));
    } else {
        ESP_LOGW(TAG, "IR force-resend queue failed");
    }
    MarkDirtyAndRefresh();
}

void MjAcPage::TogglePower() {
    state_.power = !state_.power;
    ApplyAndSend();
}

void MjAcPage::CycleMode(int delta) {
    int mode = static_cast<int>(state_.mode) + delta;
    if (mode < 0) {
        mode = 4;
    }
    if (mode > 4) {
        mode = 0;
    }
    state_.mode = static_cast<MjAcMode>(mode);
    ApplyAndSend();
}

void MjAcPage::CycleFan(int delta) {
    int fan = static_cast<int>(state_.fan) + delta;
    if (fan < 0) {
        fan = 5;
    }
    if (fan > 5) {
        fan = 0;
    }
    state_.fan = static_cast<MjAcFan>(fan);
    ApplyAndSend();
}

void MjAcPage::AdjustTemp(int delta) {
    int temp = static_cast<int>(state_.temp_c) + delta;
    if (temp < 16) {
        temp = 16;
    }
    if (temp > 30) {
        temp = 30;
    }
    state_.temp_c = static_cast<uint8_t>(temp);
    ApplyAndSend();
}

void MjAcPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }

    display_ = display;
    active_ = true;
    ESP_LOGI(TAG, "OnEnter MJ AC page protocol=%s reuse=%d", MitsubishiIrSender::ProtocolName(),
             panel_ != nullptr ? 1 : 0);
    ir_.Initialize();

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
    // HideChatUi takes its own display lock ó do not nest locks here.
    display->HideChatUi();

    last_power_ = !state_.power;
    last_temp_ = 0;
    last_mode_ = MjAcMode::Auto;
    last_fan_ = MjAcFan::Max;
    last_tx_ = true;
    last_flash_index_ = -2;
    ui_dirty_ = true;
    RefreshUi(display);
}

void MjAcPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    if (display == nullptr || panel_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void MjAcPage::Tick(CardputerAdvCarLcdDisplay* display) {
    // Use active_ ù never call LVGL without the display lock from the timer task.
    if (!active_ || panel_ == nullptr) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    const bool tx_active = now < tx_active_until_us_;
    const bool flash_on = (btn_flash_index_ >= 0 && now < btn_flash_until_us_);
    const int flash_idx = flash_on ? btn_flash_index_ : -1;

    if (ui_dirty_ || tx_active != last_tx_ || flash_idx != last_flash_index_ ||
        state_.power != last_power_ || state_.temp_c != last_temp_ ||
        state_.mode != last_mode_ || state_.fan != last_fan_) {
        RefreshUi(display);
    }
}

bool MjAcPage::HandleKey(const KeyEvent& event) {
    // Press-edge only. No fingerprint debounce (TCA8418 already de-dupes).
    if (!event.pressed) {
        return false;
    }
    if (event.is_modifier) {
        return false;
    }
    if (!active_) {
        ESP_LOGW(TAG, "HandleKey ignored ù page not active");
        return false;
    }

    const char ch = KeyCharLower(event);
    ESP_LOGI(TAG, "HandleKey code=0x%02X char='%c' power=%d temp=%u",
             event.key_code, ch ? ch : '?', state_.power, state_.temp_c);

    // Match HID code OR printable char (defensive against ADV remap quirks).
    const bool is_p = (event.key_code == KC_P) || (ch == 'p');
    const bool is_f = (event.key_code == KC_F) || (ch == 'f');
    const bool is_m = (event.key_code == KC_M) || (ch == 'm');
    const bool is_s = (event.key_code == KC_S) || (ch == 's') ||
                      (event.key_code == KC_ENTER) || (event.key_code == KC_SPACE) ||
                      (ch == ' ');
    const bool is_temp_up = (event.key_code == KC_SEMICOLON) || (event.key_code == KC_UP) ||
                            (ch == ';') || (ch == ':');
    const bool is_temp_down = (event.key_code == KC_DOT) || (event.key_code == KC_DOWN) ||
                              (ch == '.') || (ch == '>');

    if (is_p) {
        FlashButton(0);
        TogglePower();
        return true;
    }
    if (is_f) {
        FlashButton(1);
        CycleFan(1);
        return true;
    }
    if (is_temp_up) {
        FlashButton(2);
        AdjustTemp(1);
        return true;
    }
    if (is_s) {
        FlashButton(3);
        ForceResend();
        return true;
    }
    if (is_m) {
        FlashButton(4);
        CycleMode(1);
        return true;
    }
    if (is_temp_down) {
        FlashButton(5);
        AdjustTemp(-1);
        return true;
    }

    ESP_LOGW(TAG, "unhandled key code=0x%02X char='%c'", event.key_code, ch ? ch : '?');
    return false;
}
