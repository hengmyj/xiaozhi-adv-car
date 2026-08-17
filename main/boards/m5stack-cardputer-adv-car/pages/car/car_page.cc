#include "car_page.h"

#include "cardputer_adv_lcd_display.h"

namespace {

constexpr int kWheelCount = 2;
constexpr int kSpokesPerWheel = 2;

// Side-profile pickup in 170x88 scene panel (origin top-left).
constexpr int kTruckX = 8;
constexpr int kTruckY = 6;
constexpr int kWheelSize = 28;
constexpr int kWheelY = 54;
constexpr int kWheelX[kWheelCount] = {kTruckX + 24, kTruckX + 104};
constexpr int kSpinPerTick = 300;  // 30 deg/tick in LVGL 0.1 deg units

void StylePanel(lv_obj_t* obj, lv_color_t fill, lv_color_t border, int radius = 2) {
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void CarPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    EnterPage(display, "\xe9\xba\xa6\xe8\xbd\xae\xe5\xb0\x8f\xe8\xbd\xa6");
}

void CarPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    LeavePage(display);
}

void CarPage::Tick(CardputerAdvCarLcdDisplay* display) {
    TickPage(display);
}

bool CarPage::HandleKey(const KeyEvent& event) {
    return HandleKeyPage(event);
}

void CarPage::BuildSceneGraphic(lv_obj_t* parent) {
    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Wheels: fixed rim circles at axle positions — never rotated or moved.
    for (int i = 0; i < kWheelCount; ++i) {
        wheels_[i] = lv_obj_create(parent);
        lv_obj_set_size(wheels_[i], kWheelSize, kWheelSize);
        lv_obj_set_style_radius(wheels_[i], LV_RADIUS_CIRCLE, 0);
        StylePanel(wheels_[i], lv_color_hex(0x1A1A22), lv_color_hex(0xCCCCDD), LV_RADIUS_CIRCLE);
        lv_obj_set_pos(wheels_[i], kWheelX[i], kWheelY);
        lv_obj_add_flag(wheels_[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_move_background(wheels_[i]);

        // Spoke hub: transparent child of rim, pivot at wheel geometric center.
        wheel_hubs_[i] = lv_obj_create(wheels_[i]);
        lv_obj_set_size(wheel_hubs_[i], kWheelSize, kWheelSize);
        lv_obj_set_style_bg_opa(wheel_hubs_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wheel_hubs_[i], 0, 0);
        lv_obj_set_style_pad_all(wheel_hubs_[i], 0, 0);
        lv_obj_set_pos(wheel_hubs_[i], 0, 0);
        lv_obj_clear_flag(wheel_hubs_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(wheel_hubs_[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_style_transform_pivot_x(wheel_hubs_[i], kWheelSize / 2, 0);
        lv_obj_set_style_transform_pivot_y(wheel_hubs_[i], kWheelSize / 2, 0);

        // Cross spokes baked at 0/90 deg inside hub; only the hub rotates.
        constexpr int kSpokeH = kWheelSize - 6;
        constexpr int kSpokeW = 3;
        for (int s = 0; s < kSpokesPerWheel; ++s) {
            wheel_spokes_[i][s] = lv_obj_create(wheel_hubs_[i]);
            lv_obj_set_size(wheel_spokes_[i][s], kSpokeW, kSpokeH);
            lv_obj_set_style_bg_color(wheel_spokes_[i][s], lv_color_hex(0xFFE066), 0);
            lv_obj_set_style_border_width(wheel_spokes_[i][s], 0, 0);
            lv_obj_set_style_radius(wheel_spokes_[i][s], 1, 0);
            lv_obj_set_style_pad_all(wheel_spokes_[i][s], 0, 0);
            lv_obj_clear_flag(wheel_spokes_[i][s], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(wheel_spokes_[i][s], (kWheelSize - kSpokeW) / 2, 3);
            if (s == 1) {
                lv_obj_set_style_transform_pivot_x(wheel_spokes_[i][s], kSpokeW / 2, 0);
                lv_obj_set_style_transform_pivot_y(wheel_spokes_[i][s], kSpokeH / 2, 0);
                lv_obj_set_style_transform_rotation(wheel_spokes_[i][s], 900, 0);
            }
        }
    }

    cab_ = lv_obj_create(parent);
    lv_obj_set_size(cab_, 56, 36);
    StylePanel(cab_, lv_color_hex(0x3186BB), lv_color_hex(0xFFFFFF), 4);
    lv_obj_set_pos(cab_, kTruckX + 4, kTruckY + 16);

    windshield_ = lv_obj_create(parent);
    lv_obj_set_size(windshield_, 24, 20);
    StylePanel(windshield_, lv_color_hex(0x6EC8F0), lv_color_hex(0x00D4FF), 3);
    lv_obj_set_pos(windshield_, kTruckX + 32, kTruckY + 8);

    bed_ = lv_obj_create(parent);
    lv_obj_set_size(bed_, 88, 24);
    StylePanel(bed_, lv_color_hex(0x286FA0), lv_color_hex(0xFFFFFF), 3);
    lv_obj_set_pos(bed_, kTruckX + 56, kTruckY + 28);

    bed_gate_ = lv_obj_create(parent);
    lv_obj_set_size(bed_gate_, 6, 24);
    StylePanel(bed_gate_, lv_color_hex(0x1E5578), lv_color_hex(0xCCCCDD), 2);
    lv_obj_set_pos(bed_gate_, kTruckX + 138, kTruckY + 28);

    car_front_ = lv_label_create(parent);
    lv_label_set_text(car_front_, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(car_front_, lv_color_hex(0xFFE066), 0);
    lv_obj_set_pos(car_front_, kTruckX + 146, kTruckY + 20);
}

void CarPage::ResetSceneWidgets() {
    cab_ = nullptr;
    windshield_ = nullptr;
    bed_ = nullptr;
    bed_gate_ = nullptr;
    car_front_ = nullptr;
    for (int i = 0; i < kWheelCount; ++i) {
        wheels_[i] = nullptr;
        wheel_hubs_[i] = nullptr;
        for (int s = 0; s < kSpokesPerWheel; ++s) {
            wheel_spokes_[i][s] = nullptr;
        }
    }
}

void CarPage::UpdateSceneGraphic(int running) {
    if (cab_ == nullptr) {
        return;
    }

    lv_color_t cab_color = running ? lv_color_hex(0x00AA66) : lv_color_hex(0x3186BB);
    lv_color_t bed_color = running ? lv_color_hex(0x008855) : lv_color_hex(0x286FA0);
    lv_obj_set_style_bg_color(cab_, cab_color, 0);
    lv_obj_set_style_bg_color(bed_, bed_color, 0);

    const int spin = running ? scene_phase_ * kSpinPerTick : 0;
    for (int i = 0; i < kWheelCount; ++i) {
        if (wheel_hubs_[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_transform_rotation(wheel_hubs_[i], spin, 0);
        lv_obj_invalidate(wheel_hubs_[i]);
    }
}
