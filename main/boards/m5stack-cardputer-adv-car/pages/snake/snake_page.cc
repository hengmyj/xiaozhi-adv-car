#include "snake_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

#define TAG "SnakePage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr lv_coord_t kCellPx = 15;

constexpr uint32_t kHexBg = 0x081010;
constexpr uint32_t kHexSnake = 0x00CC00;
constexpr uint32_t kHexHead = 0x00FFFF;
constexpr uint32_t kHexFood = 0xFF0000;
constexpr uint32_t kHexBorder = 0x224422;

void StripStyles(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

void StyleTile(lv_obj_t* obj, uint32_t fill) {
    StripStyles(obj);
    lv_obj_set_size(obj, kCellPx, kCellPx);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(kHexBorder), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

}  // namespace

void SnakePage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
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
    food_obj_ = nullptr;
    std::memset(body_objs_, 0, sizeof(body_objs_));
    ESP_LOGI(TAG, "DestroyPanel snake heap %u->%u largest %u->%u", (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void SnakePage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
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

    food_obj_ = lv_obj_create(panel_);
    StyleTile(food_obj_, kHexFood);
    lv_obj_add_flag(food_obj_, LV_OBJ_FLAG_HIDDEN);

    hud_ = lv_label_create(panel_);
    lv_label_set_text(hud_, "SNAKE 0");
    lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hud_, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_bg_color(hud_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(hud_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(hud_, 4, 0);
    lv_obj_set_style_pad_ver(hud_, 1, 0);
    lv_obj_align(hud_, LV_ALIGN_TOP_LEFT, 2, 2);

    ESP_LOGI(TAG, "BuildPanel snake heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void SnakePage::EnsureBodyObj(uint8_t i) {
    if (i >= kMaxLen || panel_ == nullptr || body_objs_[i] != nullptr) {
        return;
    }
    body_objs_[i] = lv_obj_create(panel_);
    StyleTile(body_objs_[i], kHexSnake);
    lv_obj_add_flag(body_objs_[i], LV_OBJ_FLAG_HIDDEN);
}

bool SnakePage::Occupied(int8_t x, int8_t y, bool include_tail) const {
    const uint8_t n = include_tail ? len_ : ((len_ > 0) ? static_cast<uint8_t>(len_ - 1) : 0);
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t idx = static_cast<uint8_t>((head_ + kMaxLen - i) % kMaxLen);
        if (xs_[idx] == x && ys_[idx] == y) {
            return true;
        }
    }
    return false;
}

void SnakePage::SpawnFood() {
    if (len_ >= kMaxLen) {
        food_x_ = -1;
        food_y_ = -1;
        return;
    }
    for (int tries = 0; tries < 64; ++tries) {
        const int8_t x = static_cast<int8_t>(esp_random() % kCols);
        const int8_t y = static_cast<int8_t>(esp_random() % kRows);
        if (!Occupied(x, y)) {
            food_x_ = x;
            food_y_ = y;
            return;
        }
    }
    for (int8_t y = 0; y < kRows; ++y) {
        for (int8_t x = 0; x < kCols; ++x) {
            if (!Occupied(x, y)) {
                food_x_ = x;
                food_y_ = y;
                return;
            }
        }
    }
    food_x_ = -1;
    food_y_ = -1;
}

void SnakePage::ResetGame() {
    head_ = 0;
    len_ = 3;
    dir_ = Dir::Right;
    queued_dir_ = Dir::Right;
    score_ = 0;
    phase_ = Phase::Ready;
    const int8_t y = kRows / 2;
    xs_[0] = 4;
    ys_[0] = y;
    xs_[kMaxLen - 1] = 3;
    ys_[kMaxLen - 1] = y;
    xs_[kMaxLen - 2] = 2;
    ys_[kMaxLen - 2] = y;
    SpawnFood();
    last_step_us_ = 0;
}

void SnakePage::QueueDir(Dir d) {
    if (phase_ != Phase::Running) {
        return;
    }
    queued_dir_ = d;
}

bool SnakePage::ApplyQueuedDir() {
    const bool reverse =
        (dir_ == Dir::Up && queued_dir_ == Dir::Down) ||
        (dir_ == Dir::Down && queued_dir_ == Dir::Up) ||
        (dir_ == Dir::Left && queued_dir_ == Dir::Right) ||
        (dir_ == Dir::Right && queued_dir_ == Dir::Left);
    if (!reverse) {
        dir_ = queued_dir_;
    }
    return true;
}

void SnakePage::UpdateHud() {
    if (hud_ == nullptr) {
        return;
    }
    char buf[24];
    if (phase_ == Phase::Dead) {
        std::snprintf(buf, sizeof(buf), "DEAD %u", static_cast<unsigned>(score_));
    } else if (phase_ == Phase::Paused) {
        std::snprintf(buf, sizeof(buf), "PAUSE %u", static_cast<unsigned>(score_));
    } else if (phase_ == Phase::Ready) {
        std::snprintf(buf, sizeof(buf), "GO %u", static_cast<unsigned>(score_));
    } else {
        std::snprintf(buf, sizeof(buf), "SNAKE %u", static_cast<unsigned>(score_));
    }
    lv_label_set_text(hud_, buf);
    lv_obj_move_foreground(hud_);
}

void SnakePage::DrawBoard() {
    if (panel_ == nullptr) {
        return;
    }
    for (uint8_t i = 0; i < len_; ++i) {
        EnsureBodyObj(i);
        lv_obj_t* obj = body_objs_[i];
        if (obj == nullptr) {
            continue;
        }
        const uint8_t idx = static_cast<uint8_t>((head_ + kMaxLen - i) % kMaxLen);
        lv_obj_set_pos(obj, static_cast<lv_coord_t>(xs_[idx] * kCell),
                       static_cast<lv_coord_t>(ys_[idx] * kCell));
        lv_obj_set_style_bg_color(obj, lv_color_hex(i == 0 ? kHexHead : kHexSnake), 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    for (uint8_t i = len_; i < kMaxLen; ++i) {
        if (body_objs_[i] == nullptr) {
            break;
        }
        lv_obj_add_flag(body_objs_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (food_obj_ != nullptr) {
        if (food_x_ >= 0 && food_y_ >= 0) {
            lv_obj_set_pos(food_obj_, static_cast<lv_coord_t>(food_x_ * kCell),
                           static_cast<lv_coord_t>(food_y_ * kCell));
            lv_obj_clear_flag(food_obj_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(food_obj_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (hud_ != nullptr) {
        lv_obj_move_foreground(hud_);
    }
    // No full-panel invalidate: every set_pos above invalidates its own region.
    // Full-panel invalidate per step forced a 240x135 redraw (~13ms holding the
    // LVGL mutex) inside the esp_timer Tick -> lock timeouts -> tree stomp.
}

void SnakePage::Step() {
    if (phase_ != Phase::Running || len_ == 0) {
        return;
    }
    ApplyQueuedDir();
    int8_t nx = xs_[head_];
    int8_t ny = ys_[head_];
    switch (dir_) {
        case Dir::Up:
            ny = static_cast<int8_t>(ny - 1);
            break;
        case Dir::Down:
            ny = static_cast<int8_t>(ny + 1);
            break;
        case Dir::Left:
            nx = static_cast<int8_t>(nx - 1);
            break;
        case Dir::Right:
            nx = static_cast<int8_t>(nx + 1);
            break;
    }
    const bool eat = (nx == food_x_ && ny == food_y_);
    if (nx < 0 || nx >= kCols || ny < 0 || ny >= kRows || Occupied(nx, ny, eat)) {
        phase_ = Phase::Dead;
        return;
    }
    head_ = static_cast<uint8_t>((head_ + 1) % kMaxLen);
    xs_[head_] = nx;
    ys_[head_] = ny;
    if (eat) {
        if (len_ < kMaxLen) {
            ++len_;
        }
        ++score_;
        SpawnFood();
    }
}

void SnakePage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    stepping_ = false;
    ESP_LOGI(TAG, "OnEnter snake reuse=%d heap=%u largest=%u", panel_ != nullptr ? 1 : 0,
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
        DrawBoard();
        UpdateHud();
    }
    display->HideChatUi();
    ESP_LOGI(TAG, "OnEnter snake done heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void SnakePage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    ESP_LOGI(TAG, "OnLeave snake heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    active_ = false;
    stepping_ = false;
    DestroyPanel(display);
}

void SnakePage::ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) {
    DestroyPanel(display);
}

void SnakePage::Tick(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr || stepping_) {
        return;
    }
    if (phase_ != Phase::Running) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (last_step_us_ != 0 && (now - last_step_us_) < kStepUs) {
        return;
    }
    last_step_us_ = now;
    stepping_ = true;
    Step();
    {
        DisplayLockGuard lock(display);
        DrawBoard();
        UpdateHud();
    }
    stepping_ = false;
}

bool SnakePage::HandleKey(const KeyEvent& event) {
    if (!event.pressed || event.is_modifier || !active_) {
        return false;
    }
    const char ch = (event.key_char && event.key_char[0]) ? event.key_char[0] : '\0';
    const char lower = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;

    if (event.key_code == KC_ENTER || event.key_code == KC_SPACE || ch == ' ') {
        if (phase_ == Phase::Ready) {
            phase_ = Phase::Running;
            last_step_us_ = 0;
        } else if (phase_ == Phase::Dead) {
            ResetGame();
            phase_ = Phase::Running;
        } else if (phase_ == Phase::Paused) {
            phase_ = Phase::Running;
            last_step_us_ = 0;
        }
        if (display_ != nullptr) {
            DisplayLockGuard lock(display_);
            DrawBoard();
            UpdateHud();
        }
        return true;
    }
    if (event.key_code == KC_P || lower == 'p') {
        if (phase_ == Phase::Running) {
            phase_ = Phase::Paused;
        } else if (phase_ == Phase::Paused) {
            phase_ = Phase::Running;
            last_step_us_ = 0;
        }
        if (display_ != nullptr) {
            DisplayLockGuard lock(display_);
            UpdateHud();
        }
        return true;
    }

    Dir d = dir_;
    bool dir_key = false;
    if (event.key_code == KC_SEMICOLON || event.key_code == KC_W || lower == 'w' ||
        event.key_code == KC_UP) {
        d = Dir::Up;
        dir_key = true;
    } else if (event.key_code == KC_DOT || event.key_code == KC_S || lower == 's' ||
               event.key_code == KC_DOWN) {
        d = Dir::Down;
        dir_key = true;
    } else if (event.key_code == KC_COMMA || event.key_code == KC_A || lower == 'a' ||
               event.key_code == KC_LEFT) {
        d = Dir::Left;
        dir_key = true;
    } else if (event.key_code == KC_SLASH || event.key_code == KC_D || lower == 'd' ||
               event.key_code == KC_RIGHT) {
        d = Dir::Right;
        dir_key = true;
    }
    if (!dir_key) {
        return true;
    }
    if (phase_ == Phase::Ready) {
        phase_ = Phase::Running;
        last_step_us_ = 0;
        if (display_ != nullptr) {
            DisplayLockGuard lock(display_);
            UpdateHud();
        }
    }
    QueueDir(d);
    return true;
}
