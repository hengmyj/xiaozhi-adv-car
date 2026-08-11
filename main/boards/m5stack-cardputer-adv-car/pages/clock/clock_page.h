#pragma once

#include "page.h"

#include <lvgl.h>

class CardputerAdvCarLcdDisplay;

class ClockPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void RefreshTime(CardputerAdvCarLcdDisplay* display);

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    int last_sec_ = -1;
};
