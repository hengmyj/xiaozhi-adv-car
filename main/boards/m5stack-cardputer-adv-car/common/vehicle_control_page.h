#pragma once

#include "emqx_mqtt_client.h"
#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

class CardputerAdvCarLcdDisplay;

class VehicleControlPage : public Page {
public:
    void SetMqtt(EmqxCarMqtt* mqtt) { mqtt_ = mqtt; }
    lv_obj_t* GetRootPanel() const override { return panel_; }

protected:
    void EnterPage(CardputerAdvCarLcdDisplay* display, const char* title);
    void LeavePage(CardputerAdvCarLcdDisplay* display);
    void TickPage(CardputerAdvCarLcdDisplay* display);
    bool HandleKeyPage(const KeyEvent& event);

    virtual void BuildSceneGraphic(lv_obj_t* parent) = 0;
    virtual void UpdateSceneGraphic(int running) = 0;
    virtual int SceneAnimationIntervalUs() const { return 120000; }

    EmqxCarMqtt* mqtt_ = nullptr;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* scene_ = nullptr;
    lv_obj_t* speed_arc_ = nullptr;
    lv_obj_t* speed_label_ = nullptr;
    lv_obj_t* run_dot_ = nullptr;
    lv_obj_t* dir_left_ = nullptr;
    lv_obj_t* dir_center_ = nullptr;
    lv_obj_t* dir_right_ = nullptr;

    int last_run_ = -1;
    int last_speed_ = -1;
    int last_dir_ = -999;
    bool last_mqtt_ = false;
    int scene_phase_ = 0;
    int64_t last_scene_tick_us_ = 0;
    uint32_t last_key_fp_ = 0;
    int local_dir_ = 0;

private:
    void BuildDashboard(CardputerAdvCarLcdDisplay* display, const char* title);
    void BuildMqttSignalIcon(lv_obj_t* parent);
    void UpdateMqttSignalIcon(bool connected);
    void RefreshDashboard();
    void SendForward();
    void SendStop();
    void SendLeft();
    void SendRight();

    lv_obj_t* mqtt_signal_ = nullptr;
    lv_obj_t* mqtt_tower_top_ = nullptr;
    lv_obj_t* mqtt_bars_[4] = {};
};
