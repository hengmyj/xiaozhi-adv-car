#include "spider_page.h"

#include <cmath>

#include "cardputer_adv_lcd_display.h"

namespace {

constexpr int kSceneW = 170;
constexpr int kSceneH = 88;
constexpr int kLegCount = 4;
constexpr int kLegLen = 28;
constexpr int kLegWidth = 4;
constexpr int kLegWiggle = 80;  // 8 deg in LVGL 0.1 deg units

constexpr int kAbdomenW = 80;
constexpr int kAbdomenH = 36;

constexpr int kHeadW = 28;
constexpr int kHeadH = 14;
constexpr int kHeadOverlap = 5;

constexpr int kCyOffset = 2;

// Rim attach angles (deg, 0=right, 90=down): FL, FR, BL, BR.
constexpr int kLegAttachAnglesDeg[kLegCount] = {315, 45, 225, 135};

struct LegAttach {
    int ax;
    int ay;
    int rot_tenths;
};

LegAttach ComputeLegAttach(int i, int cx, int cy, int rx, int ry) {
    const float rad = static_cast<float>(kLegAttachAnglesDeg[i]) * static_cast<float>(M_PI) / 180.0f;
    LegAttach attach{
        cx + static_cast<int>(std::lround(rx * std::cos(rad))),
        cy + static_cast<int>(std::lround(ry * std::sin(rad))),
        (kLegAttachAnglesDeg[i] - 90) * 10,
    };
    return attach;
}

void StyleBodyPart(lv_obj_t* obj, lv_color_t fill, lv_color_t border, int radius = LV_RADIUS_CIRCLE) {
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void SpiderPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    EnterPage(display, "\xe8\x9c\x98\xe8\x9a\x81");
}

void SpiderPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    LeavePage(display);
}

void SpiderPage::Tick(CardputerAdvCarLcdDisplay* display) {
    TickPage(display);
}

bool SpiderPage::HandleKey(const KeyEvent& event) {
    return HandleKeyPage(event);
}

void SpiderPage::BuildSceneGraphic(lv_obj_t* parent) {
    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    const int cx = kSceneW / 2;
    const int cy = kSceneH / 2 + kCyOffset;
    const int rx = kAbdomenW / 2;
    const int ry = kAbdomenH / 2;

    abdomen_ = lv_obj_create(parent);
    lv_obj_set_size(abdomen_, kAbdomenW, kAbdomenH);
    StyleBodyPart(abdomen_, lv_color_hex(0x7B2FF7), lv_color_hex(0x00D4FF));
    lv_obj_set_pos(abdomen_, cx - rx, cy - ry);

    head_ = lv_obj_create(parent);
    lv_obj_set_size(head_, kHeadW, kHeadH);
    StyleBodyPart(head_, lv_color_hex(0x5A1FB8), lv_color_hex(0x00D4FF));
    lv_obj_set_pos(head_, cx - kHeadW / 2, cy - ry - kHeadH + kHeadOverlap);

    for (int i = 0; i < kLegCount; ++i) {
        const LegAttach attach = ComputeLegAttach(i, cx, cy, rx, ry);

        legs_[i] = lv_obj_create(parent);
        lv_obj_set_size(legs_[i], kLegWidth, kLegLen);
        lv_obj_set_style_bg_color(legs_[i], lv_color_hex(0xCCCCDD), 0);
        lv_obj_set_style_border_width(legs_[i], 0, 0);
        lv_obj_set_style_radius(legs_[i], 2, 0);
        lv_obj_set_style_pad_all(legs_[i], 0, 0);
        lv_obj_clear_flag(legs_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(legs_[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_pos(legs_[i], attach.ax - kLegWidth / 2, attach.ay);
        lv_obj_set_style_transform_pivot_x(legs_[i], kLegWidth / 2, 0);
        lv_obj_set_style_transform_pivot_y(legs_[i], 0, 0);
        lv_obj_set_style_transform_rotation(legs_[i], attach.rot_tenths, 0);
        lv_obj_move_foreground(legs_[i]);
    }
}

void SpiderPage::UpdateSceneGraphic(int running) {
    if (abdomen_ == nullptr) {
        return;
    }

    lv_color_t body_color = running ? lv_color_hex(0x00AA66) : lv_color_hex(0x7B2FF7);
    lv_obj_set_style_bg_color(abdomen_, body_color, 0);
    lv_obj_set_style_bg_color(head_, running ? lv_color_hex(0x008844) : lv_color_hex(0x5A1FB8), 0);

    const int cx = kSceneW / 2;
    const int cy = kSceneH / 2 + kCyOffset;
    const int rx = kAbdomenW / 2;
    const int ry = kAbdomenH / 2;

    for (int i = 0; i < kLegCount; ++i) {
        if (legs_[i] == nullptr) {
            continue;
        }
        const LegAttach attach = ComputeLegAttach(i, cx, cy, rx, ry);
        int wiggle = 0;
        if (running) {
            const bool even_phase = (scene_phase_ % 2) == 0;
            const bool pair_a = (i % 2) == 0;
            wiggle = (even_phase == pair_a) ? kLegWiggle : -kLegWiggle;
        }
        lv_obj_set_style_transform_rotation(legs_[i], attach.rot_tenths + wiggle, 0);
        lv_obj_set_style_bg_color(legs_[i], running ? lv_color_hex(0x00E676) : lv_color_hex(0xCCCCDD), 0);
        lv_obj_invalidate(legs_[i]);
    }
}
