#pragma once

#include "ir/mitsubishi_ir.h"
#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

class CardputerAdvCarLcdDisplay;

class MjAcPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void RefreshUi(CardputerAdvCarLcdDisplay* display);
    void ApplyAndSend();
    void ForceResend();
    void CycleMode(int delta);
    void CycleFan(int delta);
    void AdjustTemp(int delta);
    void TogglePower();
    void UpdateModeIcons();
    void FlashButton(int index);
    void ClearButtonFlash();
    void MarkDirtyAndRefresh();
    static char KeyCharLower(const KeyEvent& event);

    MitsubishiIrSender ir_;
    MjAcState state_{};
    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* logo_label_ = nullptr;
    lv_obj_t* mode_cool_ = nullptr;
    lv_obj_t* mode_fan_ = nullptr;
    lv_obj_t* mode_power_ = nullptr;
    lv_obj_t* fan_bar_ = nullptr;
    lv_obj_t* tx_dot_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* btns_[6] = {};

    int64_t tx_active_until_us_ = 0;
    int64_t btn_flash_until_us_ = 0;
    int btn_flash_index_ = -1;
    bool ui_dirty_ = true;

    bool last_power_ = false;
    uint8_t last_temp_ = 0;
    MjAcMode last_mode_ = MjAcMode::Cool;
    MjAcFan last_fan_ = MjAcFan::Auto;
    bool last_tx_ = false;
    int last_flash_index_ = -2;
};
