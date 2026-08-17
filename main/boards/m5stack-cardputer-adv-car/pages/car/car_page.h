#pragma once

#include "tca8418_keyboard.h"
#include "vehicle_control_page.h"

class CardputerAdvCarLcdDisplay;

class CarPage : public VehicleControlPage {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);

protected:
    void BuildSceneGraphic(lv_obj_t* parent) override;
    void UpdateSceneGraphic(int running) override;
    void ResetSceneWidgets() override;

private:
    static constexpr int kWheelCount = 2;
    static constexpr int kSpokesPerWheel = 2;
    lv_obj_t* cab_ = nullptr;
    lv_obj_t* windshield_ = nullptr;
    lv_obj_t* bed_ = nullptr;
    lv_obj_t* bed_gate_ = nullptr;
    lv_obj_t* car_front_ = nullptr;
    lv_obj_t* wheels_[kWheelCount] = {};
    lv_obj_t* wheel_hubs_[kWheelCount] = {};
    lv_obj_t* wheel_spokes_[kWheelCount][kSpokesPerWheel] = {};
};
