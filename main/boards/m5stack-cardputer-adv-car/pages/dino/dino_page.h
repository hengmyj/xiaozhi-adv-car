#pragma once

#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class CardputerAdvCarLcdDisplay;

class DinoPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    enum class Phase : uint8_t { Ready, Running, Paused, Dead };

    static constexpr lv_coord_t kGroundY = 118;   // ground line Y
    static constexpr lv_coord_t kDinoX = 24;      // dino left edge
    static constexpr int kMaxTree = 4;
    static constexpr int64_t kFrameUs = 16000;    // ~62.5 fps logic tick

    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void ResetGame();
    void StartRun();
    void SpawnTree();
    void StepFrame();
    void DrawScene();
    void UpdateHud();
    void Beep(int ms, int hz, int amp = 6000);
    void StartMusic();
    void StopMusic();
    static void MusicTask(void* arg);
    bool AabbHit(lv_coord_t ax, lv_coord_t ay, lv_coord_t aw, lv_coord_t ah,
                 lv_coord_t bx, lv_coord_t by, lv_coord_t bw, lv_coord_t bh) const;
    void SetDinoLegs(lv_coord_t lift);

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool stepping_ = false;
    TaskHandle_t music_task_ = nullptr;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* hud_ = nullptr;
    lv_obj_t* speed_label_ = nullptr;
    lv_obj_t* ground_ = nullptr;
    lv_obj_t* dino_body_ = nullptr;
    lv_obj_t* dino_head_ = nullptr;
    lv_obj_t* dino_eye_ = nullptr;
    lv_obj_t* dino_leg_a_ = nullptr;
    lv_obj_t* dino_leg_b_ = nullptr;
    lv_obj_t* tree_trunk_[kMaxTree] = {};
    lv_obj_t* tree_line_[kMaxTree] = {};  // fir-tree triangle outline

    Phase phase_ = Phase::Ready;
    uint32_t score_ = 0;
    int64_t last_frame_us_ = 0;

    // Dino physics (pixels per frame at kFrameUs)
    float dino_y_ = 0;        // offset above ground (0 = running on ground)
    float dino_vy_ = 0;       // vertical velocity
    bool jumping_ = false;

    // World scroll speed (px/frame), grows with score
    float speed_ = 3.2f;

    // Tree slots: x >= 240 means off-screen (free)
    lv_coord_t tree_x_[kMaxTree] = {240, 240, 240, 240};
    int16_t next_gap_ = 90;   // frames until next spawn
    uint8_t run_phase_ = 0;   // leg animation
};
