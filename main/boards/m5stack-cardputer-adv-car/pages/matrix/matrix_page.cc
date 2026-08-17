#include "matrix_page.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstring>

#define TAG "MatrixPage"

namespace {

constexpr lv_coord_t kScreenW = 240;
constexpr lv_coord_t kScreenH = 135;
constexpr int kCanvasW = 120;
constexpr int kCanvasH = 68;

// Tiny 5×7 glyphs (same idea as Clock) — painted into canvas cells, no LVGL labels.
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;

const char kCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%&*";

// Sparse bitmaps for A-Z / 0-9 / symbols — index via CharGlyph().
constexpr const char* kAlpha[36][kGlyphH] = {
    {".***.", "*...*", "*****", "*...*", "*...*", "*...*", "*...*"},  // A
    {"****.", "*...*", "****.", "*...*", "*...*", "*...*", "****."},
    {".****", "*....", "*....", "*....", "*....", "*....", ".****"},
    {"****.", "*...*", "*...*", "*...*", "*...*", "*...*", "****."},
    {"*****", "*....", "****.", "*....", "*....", "*....", "*****"},
    {"*****", "*....", "****.", "*....", "*....", "*....", "*...."},
    {".****", "*....", "*....", "*..**", "*...*", "*...*", ".****"},
    {"*...*", "*...*", "*****", "*...*", "*...*", "*...*", "*...*"},
    {"*****", "..*..", "..*..", "..*..", "..*..", "..*..", "*****"},
    {"..***", "...*.", "...*.", "...*.", "...*.", "*..*.", ".***."},
    {"*...*", "*..*.", "**...", "*.*..", "*..*.", "*...*", "*...*"},
    {"*....", "*....", "*....", "*....", "*....", "*....", "*****"},
    {"*...*", "**.**", "*.*.*", "*...*", "*...*", "*...*", "*...*"},
    {"*...*", "**..*", "*.*.*", "*..**", "*...*", "*...*", "*...*"},
    {".***.", "*...*", "*...*", "*...*", "*...*", "*...*", ".***."},
    {"****.", "*...*", "*...*", "****.", "*....", "*....", "*...."},
    {".***.", "*...*", "*...*", "*...*", "*.*.*", "*..*.", ".**.*"},
    {"****.", "*...*", "*...*", "****.", "*..*.", "*...*", "*...*"},
    {".****", "*....", "*....", ".***.", "....*", "....*", "****."},
    {"*****", "..*..", "..*..", "..*..", "..*..", "..*..", "..*.."},
    {"*...*", "*...*", "*...*", "*...*", "*...*", "*...*", ".***."},
    {"*...*", "*...*", "*...*", "*...*", ".*.*.", ".*.*.", "..*.."},
    {"*...*", "*...*", "*...*", "*.*.*", "*.*.*", "**.**", "*...*"},
    {"*...*", ".*.*.", "..*..", "..*..", "..*..", ".*.*.", "*...*"},
    {"*...*", ".*.*.", "..*..", "..*..", "..*..", "..*..", "..*.."},
    {"*****", "....*", "...*.", "..*..", ".*...", "*....", "*****"},
    {".***.", "*...*", "*..**", "*.*.*", "**..*", "*...*", ".***."},  // 0
    {"..*..", ".**..", "..*..", "..*..", "..*..", "..*..", "*****"},
    {".***.", "*...*", "....*", "...*.", "..*..", ".*...", "*****"},
    {"*****", "....*", "...*.", "..**.", "....*", "*...*", ".***."},
    {"...*.", "..**.", ".*.*.", "*..*.", "*****", "...*.", "...*."},
    {"*****", "*....", "****.", "....*", "....*", "*...*", ".***."},
    {"..**.", ".*...", "*....", "****.", "*...*", "*...*", ".***."},
    {"*****", "....*", "...*.", "..*..", ".*...", ".*...", ".*..."},
    {".***.", "*...*", "*...*", ".***.", "*...*", "*...*", ".***."},
    {".***.", "*...*", "*...*", ".****", "....*", "...*.", ".***."},
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

int CharGlyph(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    return -1;
}

// RGB565 greens by life band.
uint16_t ColorForLife(uint8_t life, bool is_head) {
    if (life == 0) {
        return 0x0000;
    }
    if (is_head && life > 200) {
        return 0xCFF9;  // near-white green
    }
    if (life > 160) {
        return 0x27E8;  // bright
    }
    if (life > 80) {
        return 0x1484;  // mid
    }
    return 0x0A42;  // dim
}

void PaintGlyph(uint16_t* buf, char c, int ox, int oy, uint16_t color) {
    int gi = CharGlyph(c);
    if (gi < 0 || color == 0) {
        return;
    }
    for (int r = 0; r < kGlyphH; ++r) {
        for (int col = 0; col < kGlyphW; ++col) {
            if (kAlpha[gi][r][col] != '*') {
                continue;
            }
            const int x = ox + col;
            const int y = oy + r;
            if (x >= 0 && x < kCanvasW && y >= 0 && y < kCanvasH) {
                buf[y * kCanvasW + x] = color;
            }
        }
    }
}

}  // namespace

void MatrixPage::ResetDrops() {
    tick_count_ = 0;
    rng_ = 0xA5A5u ^ static_cast<uint32_t>(esp_log_timestamp());
    for (int c = 0; c < kCols; ++c) {
        rng_ = rng_ * 1664525u + 1013904223u;
        head_y_[c] = static_cast<float>(static_cast<int>(rng_ % 90) - 70);
        speed_[c] = 1.6f + static_cast<float>(rng_ % 40) / 18.0f;
        last_row_[c] = -999;
        for (int r = 0; r < kRows; ++r) {
            life_[c][r] = 0;
            glyph_[c][r] = ' ';
        }
    }
}

void MatrixPage::DestroyPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ == nullptr || display == nullptr) {
        return;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    DisplayLockGuard lock(display);
    lv_obj_del(panel_);
    panel_ = nullptr;
    canvas_ = nullptr;
    ESP_LOGI(TAG, "DestroyPanel matrix heap %u->%u largest %u->%u (canvas BSS %u stays)",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)before_largest,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)sizeof(canvas_buf_));
}

void MatrixPage::BuildPanel(CardputerAdvCarLcdDisplay* display) {
    if (panel_ != nullptr) {
        return;
    }

    ESP_LOGI(TAG, "BuildPanel canvas %dx%d buf=%u heap=%u largest=%u", kCanvasW, kCanvasH,
             static_cast<unsigned>(sizeof(canvas_buf_)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    DisplayLockGuard lock(display);
    panel_ = lv_obj_create(display->GetScreen());
    StripStyles(panel_);
    lv_obj_set_size(panel_, kScreenW, kScreenH);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);

    canvas_ = lv_canvas_create(panel_);
    lv_canvas_set_buffer(canvas_, canvas_buf_, kCanvasW, kCanvasH, LV_COLOR_FORMAT_RGB565);
    // 2× zoom → ~240×136, fills the 240×135 screen.
    lv_img_set_zoom(canvas_, 512);
    lv_obj_align(canvas_, LV_ALIGN_TOP_LEFT, 0, 0);
    std::memset(canvas_buf_, 0, sizeof(canvas_buf_));
    lv_obj_invalidate(canvas_);
}

void MatrixPage::PaintFrame() {
    if (canvas_ == nullptr) {
        return;
    }
    std::memset(canvas_buf_, 0, sizeof(canvas_buf_));
    for (int c = 0; c < kCols; ++c) {
        const int head_row = static_cast<int>(head_y_[c] / kCellH);
        for (int r = 0; r < kRows; ++r) {
            if (life_[c][r] == 0 || glyph_[c][r] == ' ') {
                continue;
            }
            const uint16_t color = ColorForLife(life_[c][r], r == head_row);
            PaintGlyph(canvas_buf_, glyph_[c][r], c * kCellW + 2, r * kCellH, color);
        }
    }
    lv_obj_invalidate(canvas_);
}

void MatrixPage::StepAnimation(CardputerAdvCarLcdDisplay* display) {
    if (!active_ || panel_ == nullptr || display == nullptr || stepping_) {
        return;
    }
    stepping_ = true;
    ++tick_count_;

    for (int c = 0; c < kCols; ++c) {
        head_y_[c] += speed_[c];
        if (head_y_[c] > kCanvasH + kTrail * kCellH) {
            rng_ = rng_ * 1664525u + 1013904223u;
            head_y_[c] = static_cast<float>(-static_cast<int>(rng_ % 30) - 6);
            speed_[c] = 1.4f + static_cast<float>(rng_ % 45) / 16.0f;
            last_row_[c] = -999;
        }

        const int head_row = static_cast<int>(head_y_[c] / kCellH);
        if (head_row != last_row_[c] && head_row >= 0 && head_row < kRows) {
            last_row_[c] = head_row;
            rng_ = rng_ * 1664525u + 1013904223u;
            glyph_[c][head_row] = kCharset[rng_ % (sizeof(kCharset) - 1)];
            life_[c][head_row] = 255;
        }

        for (int r = 0; r < kRows; ++r) {
            if (life_[c][r] == 0) {
                continue;
            }
            const int decay = 18 + (r + c) % 5;
            if (life_[c][r] > decay) {
                life_[c][r] = static_cast<uint8_t>(life_[c][r] - decay);
            } else {
                life_[c][r] = 0;
                glyph_[c][r] = ' ';
                continue;
            }
            if (life_[c][r] > 40 && ((rng_ + c * 17 + r) % 5 == 0)) {
                rng_ = rng_ * 1664525u + 1013904223u;
                glyph_[c][r] = kCharset[rng_ % (sizeof(kCharset) - 1)];
            }
        }
    }

    // Paint off-lock, invalidate under short DisplayLock (Clock pattern).
    {
        DisplayLockGuard lock(display);
        if (active_ && canvas_ != nullptr) {
            PaintFrame();
        }
    }

    if ((tick_count_ % 50) == 0) {
        ESP_LOGI(TAG, "tick=%u heap=%u", static_cast<unsigned>(tick_count_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }
    stepping_ = false;
}

void MatrixPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    active_ = true;
    stepping_ = false;
    ESP_LOGI(TAG, "OnEnter matrix canvas reuse=%d heap=%u largest=%u", panel_ != nullptr ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    BuildPanel(display);
    if (panel_ == nullptr) {
        ESP_LOGE(TAG, "BuildPanel failed");
        active_ = false;
        return;
    }

    {
        DisplayLockGuard lock(display);
        ResetDrops();
        std::memset(canvas_buf_, 0, sizeof(canvas_buf_));
        if (canvas_ != nullptr) {
            lv_obj_invalidate(canvas_);
        }
        // Show panel BEFORE HideChatUi so we never flash WiFi-only blank.
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel_);
    }
    display->HideChatUi();
    StepAnimation(display);
    ESP_LOGI(TAG, "OnEnter done heap=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void MatrixPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    active_ = false;
    stepping_ = false;
    ESP_LOGI(TAG, "OnLeave matrix heap=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    DestroyPanel(display);
}

void MatrixPage::ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) {
    DestroyPanel(display);
}

void MatrixPage::Tick(CardputerAdvCarLcdDisplay* display) {
    StepAnimation(display);
}
