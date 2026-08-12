#pragma once

#include "page.h"

#include <lvgl.h>

#include <cstdint>

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
    void DrawClock(const char* time_buf, const char* date_buf);

    // Matches DrawClock layout in clock_page.cc (padded even for RGB565 align).
    static constexpr int kCanvasW = 182;
    static constexpr int kCanvasH = 54;

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool refreshing_ = false;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    // Static RGB565 buffer — no heap alloc on Tick.
    uint16_t canvas_buf_[kCanvasW * kCanvasH] = {};
    char last_drawn_[48] = {};
    int last_sec_ = -1;
};
