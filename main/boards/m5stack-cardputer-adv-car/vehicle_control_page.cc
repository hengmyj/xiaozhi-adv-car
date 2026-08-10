#include "vehicle_control_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_timer.h>

#include <cstdio>

namespace {

constexpr int kSceneX = 66;
constexpr int kSceneY = 16;
constexpr int kSceneW = 170;
constexpr int kSceneH = 88;

uint32_t VehicleKeyFingerprint(uint8_t key_code) {
    return 0xC0000000u | static_cast<uint32_t>(key_code);
}

}  // namespace

void VehicleControlPage::BuildMqttSignalIcon(lv_obj_t* parent) {
    mqtt_signal_ = lv_obj_create(parent);
    lv_obj_set_size(mqtt_signal_, 16, 14);
    lv_obj_set_style_bg_opa(mqtt_signal_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mqtt_signal_, 0, 0);
    lv_obj_set_style_pad_all(mqtt_signal_, 0, 0);
    lv_obj_clear_flag(mqtt_signal_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mqtt_signal_, LV_ALIGN_RIGHT_MID, -2, 0);

    mqtt_tower_top_ = lv_obj_create(mqtt_signal_);
    lv_obj_set_size(mqtt_tower_top_, 2, 3);
    lv_obj_set_style_radius(mqtt_tower_top_, 1, 0);
    lv_obj_set_style_border_width(mqtt_tower_top_, 0, 0);
    lv_obj_set_style_pad_all(mqtt_tower_top_, 0, 0);
    lv_obj_align(mqtt_tower_top_, LV_ALIGN_TOP_MID, 0, 0);

    static const int kBarH[] = {3, 5, 7, 9};
    static const int kBarX[] = {1, 5, 9, 13};
    for (int i = 0; i < 4; ++i) {
        mqtt_bars_[i] = lv_obj_create(mqtt_signal_);
        lv_obj_set_size(mqtt_bars_[i], 2, kBarH[i]);
        lv_obj_set_style_radius(mqtt_bars_[i], 1, 0);
        lv_obj_set_style_border_width(mqtt_bars_[i], 0, 0);
        lv_obj_set_style_pad_all(mqtt_bars_[i], 0, 0);
        lv_obj_set_pos(mqtt_bars_[i], kBarX[i], 14 - kBarH[i]);
    }

    UpdateMqttSignalIcon(false);
}

void VehicleControlPage::UpdateMqttSignalIcon(bool connected) {
    if (mqtt_signal_ == nullptr) {
        return;
    }

    if (connected) {
        lv_obj_set_style_bg_color(mqtt_tower_top_, lv_color_hex(0x00E676), 0);
        lv_obj_set_style_bg_opa(mqtt_tower_top_, LV_OPA_COVER, 0);
        for (int i = 0; i < 4; ++i) {
            lv_obj_set_style_bg_color(mqtt_bars_[i], lv_color_hex(0x00E676), 0);
            lv_obj_set_style_bg_opa(mqtt_bars_[i], LV_OPA_COVER, 0);
        }
        return;
    }

    lv_obj_set_style_bg_color(mqtt_tower_top_, lv_color_hex(0x662233), 0);
    lv_obj_set_style_bg_opa(mqtt_tower_top_, LV_OPA_50, 0);
    for (int i = 0; i < 4; ++i) {
        lv_color_t color = (i == 0) ? lv_color_hex(0xFF2D95) : lv_color_hex(0x333344);
        lv_opa_t opa = (i == 0) ? LV_OPA_50 : LV_OPA_30;
        lv_obj_set_style_bg_color(mqtt_bars_[i], color, 0);
        lv_obj_set_style_bg_opa(mqtt_bars_[i], opa, 0);
    }
}

void VehicleControlPage::BuildDashboard(CardputerAdvCarLcdDisplay* display, const char* title) {
    if (panel_ != nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    lv_obj_set_size(panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(panel_, 0, 0);
    lv_obj_set_style_border_width(panel_, 0, 0);
    lv_obj_set_style_pad_all(panel_, 2, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x0A0A1A), 0);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_row = lv_obj_create(panel_);
    lv_obj_set_size(title_row, LV_HOR_RES - 4, 14);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = lv_label_create(title_row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x00D4FF), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 2, 0);

    BuildMqttSignalIcon(title_row);

    speed_arc_ = lv_arc_create(panel_);
    lv_obj_set_size(speed_arc_, 56, 56);
    lv_arc_set_range(speed_arc_, 0, 100);
    lv_arc_set_bg_angles(speed_arc_, 135, 45);
    lv_arc_set_angles(speed_arc_, 135, 135);
    lv_obj_set_style_arc_width(speed_arc_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_color(speed_arc_, lv_color_hex(0x333355), LV_PART_MAIN);
    lv_obj_set_style_arc_width(speed_arc_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(speed_arc_, lv_color_hex(0x00D4FF), LV_PART_INDICATOR);
    lv_obj_remove_style(speed_arc_, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(speed_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(speed_arc_, LV_ALIGN_TOP_LEFT, 4, 18);

    speed_label_ = lv_label_create(panel_);
    lv_label_set_text(speed_label_, "0");
    lv_obj_set_style_text_color(speed_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(speed_label_, speed_arc_, LV_ALIGN_CENTER, 0, 2);

    scene_ = lv_obj_create(panel_);
    lv_obj_set_size(scene_, kSceneW, kSceneH);
    lv_obj_set_style_bg_color(scene_, lv_color_hex(0x12122A), 0);
    lv_obj_set_style_border_color(scene_, lv_color_hex(0x333355), 0);
    lv_obj_set_style_border_width(scene_, 1, 0);
    lv_obj_set_style_radius(scene_, 6, 0);
    lv_obj_set_style_pad_all(scene_, 0, 0);
    lv_obj_set_pos(scene_, kSceneX, kSceneY);
    lv_obj_clear_flag(scene_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scene_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    BuildSceneGraphic(scene_);

    lv_obj_t* status_row = lv_obj_create(panel_);
    lv_obj_set_size(status_row, LV_HOR_RES - 4, 14);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_align(status_row, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

    run_dot_ = lv_obj_create(status_row);
    lv_obj_set_size(run_dot_, 10, 10);
    lv_obj_set_style_radius(run_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(run_dot_, lv_color_hex(0xFF2D95), 0);
    lv_obj_set_style_border_width(run_dot_, 0, 0);
    lv_obj_align(run_dot_, LV_ALIGN_LEFT_MID, 4, 0);

    dir_left_ = lv_label_create(status_row);
    lv_label_set_text(dir_left_, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(dir_left_, lv_color_hex(0x333355), 0);
    lv_obj_align(dir_left_, LV_ALIGN_CENTER, -18, 0);

    dir_center_ = lv_obj_create(status_row);
    lv_obj_set_size(dir_center_, 6, 6);
    lv_obj_set_style_radius(dir_center_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dir_center_, lv_color_hex(0x666688), 0);
    lv_obj_set_style_border_width(dir_center_, 0, 0);
    lv_obj_align(dir_center_, LV_ALIGN_CENTER, 0, 0);

    dir_right_ = lv_label_create(status_row);
    lv_label_set_text(dir_right_, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(dir_right_, lv_color_hex(0x333355), 0);
    lv_obj_align(dir_right_, LV_ALIGN_CENTER, 18, 0);
}

void VehicleControlPage::RefreshDashboard() {
    if (panel_ == nullptr || mqtt_ == nullptr) {
        return;
    }

    const bool mqtt_ok = mqtt_->IsConnected();
    const CarState& st = mqtt_->car_state();
    int local_run = mqtt_->run();
    int speed = mqtt_ok && st.valid && st.speed > 0 ? st.speed : mqtt_->speed();
    int dir = local_dir_;

    if (mqtt_ok != last_mqtt_) {
        last_mqtt_ = mqtt_ok;
        UpdateMqttSignalIcon(mqtt_ok);
    }

    if (local_run != last_run_ || speed != last_speed_) {
        last_run_ = local_run;
        last_speed_ = speed;
        int arc_end = 135 + (speed * 270) / 100;
        if (arc_end > 405) {
            arc_end = 405;
        }
        lv_arc_set_angles(speed_arc_, 135, arc_end);
        lv_obj_set_style_arc_color(speed_arc_, local_run ? lv_color_hex(0x00E676) : lv_color_hex(0x00D4FF), LV_PART_INDICATOR);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", speed);
        lv_label_set_text(speed_label_, buf);

        lv_obj_set_style_bg_color(run_dot_, local_run ? lv_color_hex(0x00E676) : lv_color_hex(0xFF2D95), 0);
        UpdateSceneGraphic(local_run);
        if (!local_run) {
            scene_phase_ = 0;
        }
    }

    if (dir != last_dir_) {
        last_dir_ = dir;
        lv_obj_set_style_text_color(dir_left_, dir == 1 ? lv_color_hex(0xFF2D95) : lv_color_hex(0x333355), 0);
        lv_obj_set_style_text_color(dir_right_, dir == -1 ? lv_color_hex(0x00D4FF) : lv_color_hex(0x333355), 0);
        lv_obj_set_style_bg_color(dir_center_, dir == 0 ? lv_color_hex(0xAAAAAA) : lv_color_hex(0x333355), 0);
    }
}

void VehicleControlPage::EnterPage(CardputerAdvCarLcdDisplay* display, const char* title) {
    if (display == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    BuildDashboard(display, title);
    display->HideChatUi();
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel_);

    last_run_ = -1;
    last_speed_ = -1;
    last_dir_ = -999;
    last_mqtt_ = !mqtt_->IsConnected();
    local_dir_ = mqtt_->dir();
    RefreshDashboard();
}

void VehicleControlPage::LeavePage(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr || panel_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void VehicleControlPage::TickPage(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    int64_t now = esp_timer_get_time();
    int run = mqtt_ != nullptr ? mqtt_->run() : 0;
    if (run && now - last_scene_tick_us_ > SceneAnimationIntervalUs()) {
        last_scene_tick_us_ = now;
        scene_phase_ = (scene_phase_ + 1) % 8;
        DisplayLockGuard lock(display);
        UpdateSceneGraphic(run);
    } else if (!run && scene_phase_ != 0) {
        scene_phase_ = 0;
        DisplayLockGuard lock(display);
        UpdateSceneGraphic(0);
    }

    DisplayLockGuard lock(display);
    RefreshDashboard();
}

void VehicleControlPage::SendForward() {
    if (mqtt_ == nullptr) {
        return;
    }
    mqtt_->PublishCarCmd(1, mqtt_->speed());
}

void VehicleControlPage::SendStop() {
    if (mqtt_ == nullptr) {
        return;
    }
    local_dir_ = 0;
    mqtt_->PublishCarCmd(0, mqtt_->speed());
}

void VehicleControlPage::SendLeft() {
    if (mqtt_ == nullptr) {
        return;
    }
    local_dir_ = 1;
    mqtt_->PublishFocCmd(1, mqtt_->speed());
}

void VehicleControlPage::SendRight() {
    if (mqtt_ == nullptr) {
        return;
    }
    local_dir_ = -1;
    mqtt_->PublishFocCmd(-1, mqtt_->speed());
}

bool VehicleControlPage::HandleKeyPage(const KeyEvent& event) {
    if (!event.pressed || mqtt_ == nullptr) {
        if (!event.pressed) {
            last_key_fp_ = 0;
        }
        return false;
    }

    uint32_t fp = VehicleKeyFingerprint(event.key_code);
    if (fp == last_key_fp_) {
        return false;
    }
    last_key_fp_ = fp;

    switch (event.key_code) {
        case KC_SEMICOLON:
        case KC_UP:
            SendForward();
            return true;
        case KC_DOT:
        case KC_DOWN:
            SendStop();
            return true;
        case KC_COMMA:
        case KC_LEFT:
            SendLeft();
            return true;
        case KC_SLASH:
        case KC_RIGHT:
            SendRight();
            return true;
        default:
            return false;
    }
}
