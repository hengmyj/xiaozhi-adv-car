#pragma once

#include "page.h"

#include <lvgl.h>

#include <cstdint>

class CardputerAdvCarLcdDisplay;

class MatrixPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void ResetAnimationState();
    void StepAnimation(CardputerAdvCarLcdDisplay* display);
    void ApplyCellVisual(int c, int r, uint8_t band);

    static constexpr int kCols = 10;
    static constexpr int kRows = 11;
    static constexpr int kTrail = 9;
    // Quantized visual bands (avoid per-frame style churn / heap realloc).
    static constexpr uint8_t kBandDead = 0;
    static constexpr uint8_t kBandDim = 1;
    static constexpr uint8_t kBandMid = 2;
    static constexpr uint8_t kBandBright = 3;
    static constexpr uint8_t kBandHead = 4;

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    lv_obj_t* panel_ = nullptr;
    // Fixed label pool (created once, recycled via life_/band_).
    lv_obj_t* cells_[kCols][kRows] = {};
    uint8_t life_[kCols][kRows] = {};
    uint8_t band_[kCols][kRows] = {};
    char text_buf_[kCols][kRows][2] = {};
    float head_y_[kCols] = {};
    float speed_[kCols] = {};
    int last_row_[kCols] = {};
    uint32_t rng_ = 1;
    uint32_t tick_count_ = 0;
    uint32_t heap_at_enter_ = 0;
};
