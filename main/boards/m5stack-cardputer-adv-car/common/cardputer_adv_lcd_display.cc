#include "cardputer_adv_lcd_display.h"

#include "display/display.h"

namespace {

void HideObj(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowObj(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

CardputerAdvCarLcdDisplay::CardputerAdvCarLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                                                     esp_lcd_panel_handle_t panel,
                                                     int width, int height, int offset_x,
                                                     int offset_y, bool mirror_x, bool mirror_y,
                                                     bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
}

void CardputerAdvCarLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    if (setup_ui_callback_) {
        setup_ui_callback_();
    }
}

void CardputerAdvCarLcdDisplay::HideChatUi() {
    DisplayLockGuard lock(this);
    // Non-wechat layout: top_bar_/emoji_box_ are siblings of container_ on the
    // screen. Hiding only container_ left the WiFi icon floating on a blank
    // screen when a page panel failed to show.
    HideObj(container_);
    HideObj(status_bar_);
    HideObj(top_bar_);
    HideObj(emoji_box_);
    HideObj(preview_image_);
    HideObj(bottom_bar_);
}

void CardputerAdvCarLcdDisplay::ShowChatUi() {
    DisplayLockGuard lock(this);
    ShowObj(container_);
    ShowObj(status_bar_);
    ShowObj(top_bar_);
    ShowObj(emoji_box_);
    // preview_image_ / bottom_bar_ stay hidden until content needs them
}

bool CardputerAdvCarLcdDisplay::IsChatUiVisible() const {
    return container_ != nullptr && !lv_obj_has_flag(container_, LV_OBJ_FLAG_HIDDEN);
}
