#pragma once

#include "tca8418_keyboard.h"
#include "vehicle_control_page.h"

class CardputerAdvCarLcdDisplay;

class SpiderPage : public VehicleControlPage {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);

protected:
    void BuildSceneGraphic(lv_obj_t* parent) override;
    void UpdateSceneGraphic(int running) override;

private:
    static constexpr int kLegCount = 4;
    lv_obj_t* abdomen_ = nullptr;
    lv_obj_t* head_ = nullptr;
    lv_obj_t* legs_[kLegCount] = {};
};
