#include "dino_page.h"

#include "application.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"
#include "kid_face_icon.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "DinoPage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;

constexpr uint32_t kHexBg = 0x0A0A12;
constexpr uint32_t kHexDino = 0xFFFFFF;
constexpr uint32_t kHexEye = 0x000000;
constexpr uint32_t kHexCrown = 0x33CC55;
constexpr uint32_t kHexTrunk = 0x8B5A2B;
constexpr uint32_t kHexGround = 0x777777;
constexpr uint32_t kHexHud = 0x00FF66;
constexpr uint32_t kHexSun = 0xFFCC33;
constexpr uint32_t kHexSunRay = 0xFFAA22;
constexpr uint32_t kHexBird = 0xDDDDDD;

// Sun top-left corner + float range
constexpr lv_coord_t kSunX = 199;
constexpr lv_coord_t kSunBaseY = 26;
constexpr lv_coord_t kBirdY = 45;  // sky lane below the sun

// Seagull outline, two wing poses (flap animation).
const lv_point_precise_t kBirdUp[] = {{0, 0}, {5, 7}, {10, 0}};
const lv_point_precise_t kBirdDown[] = {{0, 7}, {5, 0}, {10, 7}};

constexpr lv_coord_t kDinoW = 20;   // body+head bounding width
constexpr lv_coord_t kDinoH = 16;   // body height (head sticks up)
constexpr lv_coord_t kTrunkW = 4;
constexpr lv_coord_t kTrunkH = 8;    // fixed trunk height
constexpr lv_coord_t kTreeW = 18;    // triangle base / collision width
constexpr lv_coord_t kTreeTriH = 20; // triangle height
constexpr lv_coord_t kTreeTotalH = kTrunkH + kTreeTriH;  // 28

// Fir-tree triangle outline (relative to the line's origin), closed.
// lv_line uses lv_point_precise_t (float) points in LVGL 8.3.
const lv_point_precise_t kTriPoints[] = {
    {static_cast<lv_value_precise_t>(kTreeW) / 2, 0},
    {0, static_cast<lv_value_precise_t>(kTreeTriH)},
    {static_cast<lv_value_precise_t>(kTreeW), static_cast<lv_value_precise_t>(kTreeTriH)},
    {static_cast<lv_value_precise_t>(kTreeW) / 2, 0},
};

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

void StyleRect(lv_obj_t* obj, uint32_t fill, lv_coord_t w, lv_coord_t h) {
    StripStyles(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

}  // namespace

void DinoPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr) {
        return;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (display != nullptr) {
        DisplayLockGuard lock(display);
        lv_obj_del(panel_);
    }
    panel_ = nullptr;
    hud_ = nullptr;
    timer_label_ = nullptr;
    speed_label_ = nullptr;
    ground_ = nullptr;
    sun_ = nullptr;
    std::memset(sun_ray_, 0, sizeof(sun_ray_));
    bird_ = nullptr;
    icon_ = nullptr;
    dino_body_ = nullptr;
    dino_head_ = nullptr;
    dino_eye_ = nullptr;
    dino_leg_a_ = nullptr;
    dino_leg_b_ = nullptr;
    std::memset(tree_trunk_, 0, sizeof(tree_trunk_));
    std::memset(tree_line_, 0, sizeof(tree_line_));
    ESP_LOGI(TAG, "DestroyPanel dino heap %u->%u largest %u->%u", (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void DinoPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ != nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    StripStyles(panel_);
    lv_obj_set_size(panel_, kScreenW, kScreenH);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(kHexBg), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);

    ground_ = lv_obj_create(panel_);
    StyleRect(ground_, kHexGround, kScreenW, 2);
    lv_obj_set_pos(ground_, 0, kGroundY);

    dino_body_ = lv_obj_create(panel_);
    StyleRect(dino_body_, kHexDino, 14, 11);

    dino_head_ = lv_obj_create(panel_);
    StyleRect(dino_head_, kHexDino, 9, 7);

    dino_eye_ = lv_obj_create(panel_);
    StyleRect(dino_eye_, kHexEye, 2, 2);

    dino_leg_a_ = lv_obj_create(panel_);
    StyleRect(dino_leg_a_, kHexDino, 5, 5);

    dino_leg_b_ = lv_obj_create(panel_);
    StyleRect(dino_leg_b_, kHexDino, 5, 5);

    for (int i = 0; i < kMaxTree; ++i) {
        tree_trunk_[i] = lv_obj_create(panel_);
        StyleRect(tree_trunk_[i], kHexTrunk, kTrunkW, kTrunkH);
        tree_line_[i] = lv_line_create(panel_);
        lv_obj_set_style_line_width(tree_line_[i], 2, 0);
        lv_obj_set_style_line_color(tree_line_[i], lv_color_hex(kHexCrown), 0);
        lv_obj_set_style_line_rounded(tree_line_[i], true, 0);
        lv_line_set_points(tree_line_[i], kTriPoints, 4);
        lv_obj_add_flag(tree_trunk_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tree_line_[i], LV_OBJ_FLAG_HIDDEN);
    }

    hud_ = lv_label_create(panel_);
    lv_label_set_text(hud_, "DINO 0");
    lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hud_, lv_color_hex(kHexHud), 0);
    lv_obj_set_style_bg_color(hud_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(hud_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(hud_, 4, 0);
    lv_obj_set_style_pad_ver(hud_, 1, 0);
    lv_obj_align(hud_, LV_ALIGN_TOP_LEFT, 2, 2);

    timer_label_ = lv_label_create(panel_);
    lv_label_set_text(timer_label_, "00:00");
    lv_obj_set_style_text_font(timer_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(timer_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(timer_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(timer_label_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(timer_label_, 6, 0);
    lv_obj_set_style_pad_ver(timer_label_, 1, 0);
    lv_obj_align(timer_label_, LV_ALIGN_TOP_MID, 0, 2);

    speed_label_ = lv_label_create(panel_);
    lv_label_set_text(speed_label_, "SPD 3.2");
    lv_obj_set_style_text_font(speed_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(speed_label_, lv_color_hex(0xFFCC33), 0);
    lv_obj_set_style_bg_color(speed_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(speed_label_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(speed_label_, 4, 0);
    lv_obj_set_style_pad_ver(speed_label_, 1, 0);
    lv_obj_align(speed_label_, LV_ALIGN_TOP_RIGHT, -4, 2);

    // Sun with four rays, floating in the sky below the speed label.
    sun_ = lv_obj_create(panel_);
    StripStyles(sun_);
    lv_obj_set_size(sun_, 12, 12);
    lv_obj_set_style_radius(sun_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun_, lv_color_hex(kHexSun), 0);
    lv_obj_set_style_bg_opa(sun_, LV_OPA_COVER, 0);
    for (int i = 0; i < 4; ++i) {
        sun_ray_[i] = lv_obj_create(panel_);
        StyleRect(sun_ray_[i], kHexSunRay, 3, 3);
    }

    // Seagull (two-stroke outline), hidden until it flies by.
    bird_ = lv_line_create(panel_);
    lv_obj_set_style_line_width(bird_, 2, 0);
    lv_obj_set_style_line_color(bird_, lv_color_hex(kHexBird), 0);
    lv_obj_set_style_line_rounded(bird_, true, 0);
    lv_line_set_points(bird_, kBirdUp, 3);
    lv_obj_add_flag(bird_, LV_OBJ_FLAG_HIDDEN);

    // Kid-face logo (120x90 full-color RGB565 from rr.png) — shown in Ready/Dead.
    // Data lives in flash (kKidFaceIcon in kid_face_icon.h); lv_image_set_src() renders it
    // directly as RGB565 pixels — no runtime decode, no RAM buffer needed.
    icon_ = lv_image_create(panel_);
    lv_image_set_src(icon_, &kKidFaceIcon);
    lv_img_set_zoom(icon_, 256);  // 1x -> 120x90 (1:1, crisp)
    lv_obj_align(icon_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(icon_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "BuildPanel dino heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void DinoPage::ResetGame() {
    score_ = 0;
    speed_ = 3.2f;
    dino_y_ = 0;
    dino_vy_ = 0;
    jumping_ = false;
    run_phase_ = 0;
    next_gap_ = 60;
    for (int i = 0; i < kMaxTree; ++i) {
        tree_x_[i] = kScreenW;
    }
    bird_x_ = 240;
    bird_timer_ = static_cast<int16_t>(50 + (esp_random() % 100));  // 5-15s
    bird_flap_ = 0;
    phase_ = Phase::Ready;
    last_frame_us_ = 0;
}

void DinoPage::StartRun() {
    phase_ = Phase::Running;
    last_frame_us_ = 0;
    run_start_us_ = esp_timer_get_time();
    timer_secs_ = 0;
}

void DinoPage::SpawnTree() {
    for (int i = 0; i < kMaxTree; ++i) {
        if (tree_x_[i] >= kScreenW) {
            tree_x_[i] = kScreenW;  // right edge; scrolls in next frame
            break;
        }
    }
}

bool DinoPage::AabbHit(lv_coord_t ax, lv_coord_t ay, lv_coord_t aw, lv_coord_t ah,
                       lv_coord_t bx, lv_coord_t by, lv_coord_t bw, lv_coord_t bh) const {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void DinoPage::StepFrame() {
    if (phase_ != Phase::Running) {
        return;  // Ready/Paused/Dead: world stops, game over is a freeze + HUD
    }
    ++score_;

    // Tree spawn pacing: gap shrinks as speed grows.
    if (--next_gap_ <= 0) {
        SpawnTree();
        next_gap_ = static_cast<int16_t>(40 + (esp_random() % 40) - static_cast<uint16_t>(speed_ * 4));
        if (next_gap_ < 18) {
            next_gap_ = 18;
        }
    }

    // Scroll trees.
    for (int i = 0; i < kMaxTree; ++i) {
        if (tree_x_[i] <= kScreenW) {
            tree_x_[i] -= static_cast<lv_coord_t>(speed_);
            if (tree_x_[i] + kTreeW < 0) {
                tree_x_[i] = kScreenW;
            }
        }
    }

    // Bird is updated in Tick() (all phases) — see UpdateBird.

    // Dino jump physics.
    if (jumping_) {
        dino_vy_ += 0.45f;  // gravity
        dino_y_ += dino_vy_;
        if (dino_y_ >= 0) {
            dino_y_ = 0;
            dino_vy_ = 0;
            jumping_ = false;
        }
    }
    ++run_phase_;

    // Collision: dino hitbox vs whole tree (crown is the wide part).
    const lv_coord_t dy = static_cast<lv_coord_t>(dino_y_);  // negative = up
    const lv_coord_t dino_ax = kDinoX + 2;
    // Body top; body sits 4px above ground (matches DrawScene).
    const lv_coord_t dino_ay = kGroundY - 15 + dy;
    const lv_coord_t dino_aw = kDinoW - 4;
    const lv_coord_t dino_ah = 11;

    for (int i = 0; i < kMaxTree; ++i) {
        if (tree_x_[i] >= kScreenW) {
            continue;
        }
        const lv_coord_t ty = kGroundY - kTreeTotalH;  // treetop
        if (AabbHit(dino_ax, dino_ay, dino_aw, dino_ah, tree_x_[i], ty, kTreeW - 2,
                    kTreeTotalH)) {
            phase_ = Phase::Dead;
            Beep(120, 440, 4000);  // soft "bweep"
            Beep(240, 196, 4000);  // low gentle "boo"
            return;
        }
    }

    // Speed ramps with distance (reach top speed in ~2min instead of 5).
    speed_ = 3.2f + static_cast<float>(score_) * 0.0025f;
    if (speed_ > 8.0f) {
        speed_ = 8.0f;
    }
}

void DinoPage::UpdateBird() {
    // Runs on every Tick (Ready/Paused too, so the sky is alive).
    const float bird_speed = (phase_ == Phase::Running) ? speed_ : 3.2f;
    if (bird_x_ == kScreenW) {  // idle marker exactly at right edge
        if (--bird_timer_ <= 0) {
            bird_x_ = kScreenW - 1;  // enter flying lane next frame (not idle)
            bird_timer_ = static_cast<int16_t>(50 + (esp_random() % 100));  // 5-15s
        }
    } else {
        bird_x_ -= static_cast<lv_coord_t>(bird_speed * 1.6f);  // 1.6x tree speed
        ++bird_flap_;
        if (bird_x_ + 24 < 0) {
            bird_x_ = kScreenW;
            bird_timer_ = static_cast<int16_t>(50 + (esp_random() % 100));  // 5-15s
        }
    }
}

void DinoPage::Beep(int ms, int hz, int amp) {
    // Serialize all speaker writes (jump sfx, death sfx, BGM task).
    static SemaphoreHandle_t mux = nullptr;
    if (mux == nullptr) {
        mux = xSemaphoreCreateMutex();
        if (mux == nullptr) {
            return;
        }
    }
    if (xSemaphoreTake(mux, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;  // another tone still playing; skip this one
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        xSemaphoreGive(mux);
        return;
    }
    const int rate = codec->output_sample_rate();
    const int block = rate > 0 ? rate / 50 : 0;  // 20 ms per block
    if (block <= 0) {
        xSemaphoreGive(mux);
        return;
    }
    static std::vector<int16_t> buf;  // reused across beeps, no per-beep alloc
    buf.resize(static_cast<size_t>(block));
    const int blocks = (ms * rate / 1000) / block;
    if (blocks <= 0) {
        xSemaphoreGive(mux);
        return;
    }
    double phase = 0.0;
    const double step = 2.0 * M_PI * hz / rate;
    const double total = static_cast<double>(blocks * block);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            // Soft exponential decay: pluck-like, no harsh square sustain.
            const double t = static_cast<double>(b * block + i) / total;
            const double env = std::exp(-2.4 * t);
            buf[static_cast<size_t>(i)] = static_cast<int16_t>(amp * env * std::sin(phase));
            phase += step;
        }
        codec->OutputData(buf);
        Application::GetInstance().GetAudioService().NotifyOutputActivity();
    }
    xSemaphoreGive(mux);
}

void DinoPage::MusicTask(void* arg) {
    auto* page = static_cast<DinoPage*>(arg);
    // Gentle C-major arpeggio, slow and laid-back.
    static const int kMelody[] = {262, 330, 392, 523, 659, 523, 392, 330};
    const int n = static_cast<int>(sizeof(kMelody) / sizeof(kMelody[0]));
    int idx = 0;
    while (true) {
        // Play in Ready and Running; hold during Paused/Dead.
        if (!page->active_ || (page->phase_ != Phase::Ready && page->phase_ != Phase::Running)) {
            vTaskDelay(pdMS_TO_TICKS(50));  // idle in Paused/Dead
            continue;
        }
        page->Beep(200, kMelody[idx], 3000);
        idx = (idx + 1) % n;
        vTaskDelay(pdMS_TO_TICKS(120));  // long breath between notes
    }
}

void DinoPage::StartMusic() {
    if (music_task_ != nullptr) {
        return;
    }
    if (xTaskCreate(MusicTask, "dino_music", 4096, this, 5, &music_task_) != pdPASS) {
        ESP_LOGW(TAG, "music task create failed (low mem?)");
        music_task_ = nullptr;
    }
}

void DinoPage::StopMusic() {
    if (music_task_ != nullptr) {
        vTaskDelete(music_task_);
        music_task_ = nullptr;
    }
}

void DinoPage::SetDinoLegs(lv_coord_t lift) {
    if (dino_leg_a_ == nullptr || dino_leg_b_ == nullptr) {
        return;
    }
    const lv_coord_t foot_y = kGroundY + static_cast<lv_coord_t>(dino_y_);
    const lv_coord_t base = foot_y - 5;  // planted leg top; feet never go below ground
    const bool a_up = (run_phase_ / 4) % 2 == 0;
    lv_obj_set_pos(dino_leg_a_, kDinoX + 3, base - (a_up ? lift : 0));
    lv_obj_set_pos(dino_leg_b_, kDinoX + 9, base - (a_up ? 0 : lift));
}

void DinoPage::DrawScene() {
    if (panel_ == nullptr) {
        return;
    }
    const lv_coord_t dy = static_cast<lv_coord_t>(dino_y_);  // negative = up
    const bool running = !jumping_ && phase_ == Phase::Running;
    const lv_coord_t bob = running ? (lv_coord_t)((run_phase_ / 4) % 2) : 0;
    // Body floats 4px above ground so the legs are visible under it.
    const lv_coord_t body_bottom = kGroundY + dy - bob - 4;

    lv_obj_set_pos(dino_body_, kDinoX, body_bottom - 11);
    lv_obj_set_pos(dino_head_, kDinoX + 8, body_bottom - 17);
    lv_obj_set_pos(dino_eye_, kDinoX + 13, body_bottom - 15);
    SetDinoLegs(running ? (bob ? 4 : 0) : 0);

    for (int i = 0; i < kMaxTree; ++i) {
        lv_obj_t* trunk = tree_trunk_[i];
        lv_obj_t* line = tree_line_[i];
        if (tree_x_[i] >= kScreenW) {
            lv_obj_add_flag(trunk, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const lv_coord_t ty = kGroundY - kTreeTotalH;  // treetop
        lv_obj_set_pos(trunk, tree_x_[i] + (kTreeW - kTrunkW) / 2, ty + kTreeTriH);
        lv_obj_set_pos(line, tree_x_[i], ty);
        lv_obj_clear_flag(trunk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
    }

    // Sun floats gently (1px bob).
    const lv_coord_t sun_y = kSunBaseY + ((tick_count_ / 10) % 2 ? 1 : 0);
    lv_obj_set_pos(sun_, kSunX, sun_y);
    constexpr lv_coord_t kRayOff = 7;
    lv_obj_set_pos(sun_ray_[0], kSunX - kRayOff, sun_y - kRayOff);
    lv_obj_set_pos(sun_ray_[1], kSunX + 12, sun_y - kRayOff);
    lv_obj_set_pos(sun_ray_[2], kSunX - kRayOff, sun_y + 12);
    lv_obj_set_pos(sun_ray_[3], kSunX + 12, sun_y + 12);

    // Bird: two-stroke seagull, wings flapping, fast across the sky.
    if (bird_x_ >= kScreenW) {
        lv_obj_add_flag(bird_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(bird_, bird_x_, kBirdY);
        lv_line_set_points(bird_, (bird_flap_ / 6) % 2 ? kBirdDown : kBirdUp, 3);
        lv_obj_clear_flag(bird_, LV_OBJ_FLAG_HIDDEN);
    }

    // No full-panel invalidate: every set_pos/set_size above already invalidates
    // its own region. Full-panel invalidate forces a ~13ms 240x135 redraw per
    // frame, keeps the LVGL mutex busy, and made the esp_timer Tick lock time
    // out -> LVGL tree stomped without the lock -> world freezes after ~2s.
}

void DinoPage::UpdateHud() {
    if (hud_ == nullptr) {
        return;
    }
    char buf[24];
    switch (phase_) {
        case Phase::Ready:
            std::snprintf(buf, sizeof(buf), "DINO %u SPACE", static_cast<unsigned>(score_));
            break;
        case Phase::Paused:
            std::snprintf(buf, sizeof(buf), "PAUSE %u", static_cast<unsigned>(score_));
            break;
        case Phase::Dead:
            std::snprintf(buf, sizeof(buf), "GAME OVER %u", static_cast<unsigned>(score_));
            break;
        default:
            std::snprintf(buf, sizeof(buf), "DINO %u", static_cast<unsigned>(score_));
            break;
    }
    lv_label_set_text(hud_, buf);
    lv_obj_move_foreground(hud_);

    // Elapsed timer (freezes on pause/death, resets on each StartRun).
    if (phase_ == Phase::Running) {
        timer_secs_ = static_cast<uint32_t>((esp_timer_get_time() - run_start_us_) / 1000000);
    }
    if (timer_label_ != nullptr) {
        char tbuf[16];
        std::snprintf(tbuf, sizeof(tbuf), "%02u:%02u",
                      static_cast<unsigned>((timer_secs_ / 60) % 100),
                      static_cast<unsigned>(timer_secs_ % 60));
        lv_label_set_text(timer_label_, tbuf);
        lv_obj_move_foreground(timer_label_);
    }

    if (speed_label_ != nullptr) {
        char sbuf[16];
        std::snprintf(sbuf, sizeof(sbuf), "SPD %.1f", speed_);
        lv_label_set_text(speed_label_, sbuf);
        lv_obj_move_foreground(speed_label_);
    }

    // Kid-face logo: standby & game-over screen, hidden mid-run.
    if (icon_ != nullptr) {
        if (phase_ == Phase::Ready || phase_ == Phase::Dead) {
            lv_obj_clear_flag(icon_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(icon_);
            lv_obj_invalidate(icon_);
        } else {
            lv_obj_add_flag(icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DinoPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    stepping_ = false;
    ESP_LOGI(TAG, "OnEnter dino reuse=%d heap=%u largest=%u", panel_ != nullptr ? 1 : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        active_ = false;
        return;
    }
    ResetGame();
    {
        DisplayLockGuard lock(display);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
        DrawScene();
        UpdateHud();
    }
    display->HideChatUi();
    StartMusic();
    ESP_LOGI(TAG, "OnEnter dino done heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void DinoPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    ESP_LOGI(TAG, "OnLeave dino heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    active_ = false;
    stepping_ = false;
    StopMusic();
    DestroyPanel(display);
}

void DinoPage::ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) {
    DestroyPanel(display);
}

void DinoPage::Tick(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr || stepping_) {
        return;
    }
    ++tick_count_;
    const int64_t now = esp_timer_get_time();
    if (last_frame_us_ != 0 && (now - last_frame_us_) < kFrameUs) {
        return;
    }
    last_frame_us_ = now;
    stepping_ = true;
    StepFrame();
    UpdateBird();
    {
        DisplayLockGuard lock(display);
        DrawScene();
        UpdateHud();
    }
    stepping_ = false;
}

bool DinoPage::HandleKey(const KeyEvent& event) {
    if (!event.pressed || event.is_modifier || !active_) {
        return false;
    }
    const char ch = (event.key_char && event.key_char[0]) ? event.key_char[0] : '\0';
    const char lower = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;

    const bool jump_key = (event.key_code == KC_SPACE || event.key_code == KC_ENTER ||
                           event.key_code == KC_UP || lower == 'w' || lower == 'j');
    const bool pause_key = (event.key_code == KC_P || lower == 'p');

    if (pause_key) {
        if (phase_ == Phase::Running) {
            phase_ = Phase::Paused;
        } else if (phase_ == Phase::Paused) {
            phase_ = Phase::Running;
            last_frame_us_ = 0;
        }
        if (display_ != nullptr) {
            DisplayLockGuard lock(display_);
            UpdateHud();
        }
        return true;
    }

    if (jump_key) {
        if (phase_ == Phase::Ready) {
            StartRun();
        } else if (phase_ == Phase::Dead) {
            ResetGame();
            StartRun();
        } else if (phase_ == Phase::Running && !jumping_) {
            jumping_ = true;
            dino_vy_ = -6.6f;  // apex ~48px, plenty over the 28px trees
            dino_y_ = -0.1f;
            Beep(80, 523, 4000);  // soft chirp on jump (C5)
        }
        if (display_ != nullptr) {
            DisplayLockGuard lock(display_);
            DrawScene();
            UpdateHud();
        }
        return true;
    }
    return true;
}
