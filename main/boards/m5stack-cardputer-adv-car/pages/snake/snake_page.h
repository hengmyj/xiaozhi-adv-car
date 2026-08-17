#pragma once

#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

#include <cstdint>

class CardputerAdvCarLcdDisplay;

class SnakePage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    enum class Dir : uint8_t { Up, Down, Left, Right };
    enum class Phase : uint8_t { Ready, Running, Paused, Dead };

    static constexpr int kCols = 16;
    static constexpr int kRows = 9;
    static constexpr int kCell = 15;  // 16*15 x 9*15 = 240x135
    static constexpr int kMaxLen = kCols * kRows;
    static constexpr int kStepUs = 180000;

    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void EnsureBodyObj(uint8_t i);
    void ResetGame();
    void SpawnFood();
    bool Occupied(int8_t x, int8_t y, bool include_tail = true) const;
    void Step();
    void DrawBoard();
    void UpdateHud();
    void QueueDir(Dir d);
    bool ApplyQueuedDir();

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool stepping_ = false;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* hud_ = nullptr;
    lv_obj_t* food_obj_ = nullptr;
    lv_obj_t* body_objs_[kMaxLen] = {};

    int8_t xs_[kMaxLen] = {};
    int8_t ys_[kMaxLen] = {};
    uint8_t head_ = 0;
    uint8_t len_ = 0;
    int8_t food_x_ = 0;
    int8_t food_y_ = 0;
    Dir dir_ = Dir::Right;
    Dir queued_dir_ = Dir::Right;
    Phase phase_ = Phase::Ready;
    uint16_t score_ = 0;
    int64_t last_step_us_ = 0;
};
