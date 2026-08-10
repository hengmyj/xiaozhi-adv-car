#pragma once

#include "display/lcd_display.h"

#include <functional>
#include <lvgl.h>

class CardputerAdvCarLcdDisplay : public SpiLcdDisplay {
public:
    CardputerAdvCarLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                              int width, int height, int offset_x, int offset_y,
                              bool mirror_x, bool mirror_y, bool swap_xy);

    void SetSetupUiCallback(std::function<void()> callback) { setup_ui_callback_ = std::move(callback); }
    void HideChatUi();
    void ShowChatUi();
    lv_obj_t* GetScreen() const { return lv_screen_active(); }

    void SetupUI() override;

private:
    std::function<void()> setup_ui_callback_;
};
