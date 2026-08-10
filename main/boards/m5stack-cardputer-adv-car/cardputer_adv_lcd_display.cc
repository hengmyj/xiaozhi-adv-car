#include "cardputer_adv_lcd_display.h"

#include "display/display.h"

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
    if (container_ != nullptr) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CardputerAdvCarLcdDisplay::ShowChatUi() {
    DisplayLockGuard lock(this);
    if (container_ != nullptr) {
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_clear_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
