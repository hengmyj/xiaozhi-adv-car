#pragma once

#include "page.h"

#include <lvgl.h>

#include <cstdint>

class CardputerAdvCarLcdDisplay;

// Matrix rain via ONE RGB565 canvas (Clock-style) — no 100+ labels on no-PSRAM.
class MatrixPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void ResetDrops();
    void StepAnimation(CardputerAdvCarLcdDisplay* display);
    void PaintFrame();

    // Half-res canvas + 2x zoom → 240×136; ~16KB BSS (safe on 8MB / no-PSRAM).
    static constexpr int kCols = 12;
    static constexpr int kRows = 9;
    static constexpr int kCellW = 10;
    static constexpr int kCellH = 7;
    static constexpr int kCanvasW = 120;  // even for RGB565 row align
    static constexpr int kCanvasH = 68;
    static constexpr int kTrail = 7;

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool stepping_ = false;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    uint16_t canvas_buf_[kCanvasW * kCanvasH] = {};

    uint8_t life_[kCols][kRows] = {};
    char glyph_[kCols][kRows] = {};
    float head_y_[kCols] = {};
    float speed_[kCols] = {};
    int last_row_[kCols] = {};
    uint32_t rng_ = 1;
    uint32_t tick_count_ = 0;
};
