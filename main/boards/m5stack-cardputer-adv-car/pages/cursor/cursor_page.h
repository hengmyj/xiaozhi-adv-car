#pragma once

#include "page.h"

#include <lvgl.h>

#include <cstdint>
#include <vector>

class CardputerAdvCarLcdDisplay;

class CursorPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void UpdateBars(CardputerAdvCarLcdDisplay* display);
    void CaptureMic();
    void ReleaseMicExclusive();

    static constexpr int kBarCount = 24;
    static constexpr int kMicSamples = 512;

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool mic_exclusive_ = false;
    bool saved_wake_word_ = false;
    bool saved_voice_proc_ = false;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* bars_[kBarCount] = {};
    float levels_[kBarCount] = {};
    float peak_ = 1.0f;
    std::vector<int16_t> mic_buf_;
};
