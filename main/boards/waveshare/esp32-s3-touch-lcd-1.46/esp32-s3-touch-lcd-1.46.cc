#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "display/pet_eye_face.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "assets/lang_config.h"
#include "mcp_server.h"

#include <driver/gpio.h>
#include <driver/uart.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_lcd_touch_spd2010.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "esp_io_expander_tca9554.h"
#include "lcd_display.h"
#include <iot_button.h>

#define TAG "waveshare_lcd_1_46"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

constexpr uint16_t SPD2010_TOUCH_ADDR = 0x53;
constexpr uint32_t TP_RST_EXPANDER_PIN = IO_EXPANDER_PIN_NUM_0;   // Waveshare TCA9554_EXIO1
constexpr uint32_t LCD_RST_EXPANDER_PIN = IO_EXPANDER_PIN_NUM_1;  // Waveshare TCA9554_EXIO2
constexpr size_t kSerialTokenCapacity = 32;
constexpr size_t kExternalUartRxBufferSize = 1024;
constexpr size_t kExternalUartTxBufferSize = 1024;
constexpr uint32_t kExternalUartDuplicateSuppressMs = 10000;

bool StringIsOneOf(const char* value, const char* a, const char* b = nullptr,
                   const char* c = nullptr, const char* d = nullptr)
{
    if (value == nullptr) {
        return false;
    }
    if (a != nullptr && std::strcmp(value, a) == 0) {
        return true;
    }
    if (b != nullptr && std::strcmp(value, b) == 0) {
        return true;
    }
    if (c != nullptr && std::strcmp(value, c) == 0) {
        return true;
    }
    if (d != nullptr && std::strcmp(value, d) == 0) {
        return true;
    }
    return false;
}

char ToUpperAscii(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return static_cast<char>(ch - 'a' + 'A');
    }
    return ch;
}

bool TokenEqualsIgnoreCase(const char* value, const char* expected)
{
    if (value == nullptr || expected == nullptr) {
        return false;
    }

    while (*value != '\0' && *expected != '\0') {
        if (ToUpperAscii(*value) != ToUpperAscii(*expected)) {
            return false;
        }
        ++value;
        ++expected;
    }
    return *value == '\0' && *expected == '\0';
}

bool IsSerialTokenDelimiter(char ch)
{
    return ch == '\0' || ch == '\r' || ch == '\n' || ch == '\t' ||
           ch == ' ' || ch == ',' || ch == ';' || ch == '|';
}

bool ParsePrefixedFloatToken(const char* token, char prefix, float* value)
{
    if (token == nullptr || value == nullptr) {
        return false;
    }

    const char* cursor = token;
    while (*cursor == '"' || *cursor == '\'' || *cursor == '{' ||
           *cursor == '[' || *cursor == '(') {
        ++cursor;
    }
    if (ToUpperAscii(*cursor) != ToUpperAscii(prefix)) {
        return false;
    }
    ++cursor;
    while (*cursor == '"' || *cursor == '\'' || *cursor == ':' || *cursor == '=') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return false;
    }

    char* end = nullptr;
    float parsed = std::strtof(cursor, &end);
    if (end == cursor) {
        return false;
    }

    while (*end == '"' || *end == '\'' || *end == '}' ||
           *end == ']' || *end == ')') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

bool ParseFaceTrackToken(const char* token, float* camera_x)
{
    if (!ParsePrefixedFloatToken(token, 'T', camera_x)) {
        return false;
    }
    return true;
}

bool ParseCoordinateToken(const char* token, char axis, float* value)
{
    if (!ParsePrefixedFloatToken(token, axis, value)) {
        return false;
    }
    return true;
}

} // namespace

// 在waveshare_lcd_1_46类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
private:
    PetEyeFace pet_eye_;
    PetEyeFace::FaceMode base_face_mode_ = PetEyeFace::FaceMode::Normal;
    PetEyeFace::LookMode base_look_mode_ = PetEyeFace::LookMode::Auto;
    bool face_tracking_active_ = false;
    bool touch_active_ = false;

    void ApplyFace(PetEyeFace::FaceMode face_mode, PetEyeFace::LookMode look_mode) {
        pet_eye_.SetLookMode(look_mode);
        pet_eye_.SetFaceMode(face_mode);
    }

    void ApplyBaseFace() {
        if (touch_active_) {
            ApplyFace(PetEyeFace::FaceMode::TouchAngry, PetEyeFace::LookMode::Center);
            return;
        }

        ApplyFace(base_face_mode_, base_look_mode_);
    }

    void SetBaseFace(PetEyeFace::FaceMode face_mode, PetEyeFace::LookMode look_mode) {
        base_face_mode_ = face_mode;
        base_look_mode_ = face_tracking_active_ ? PetEyeFace::LookMode::Track : look_mode;
        ApplyBaseFace();
    }

    void SetBaseEmotion(const char* emotion) {
        if (emotion == nullptr) {
            return;
        }

        if (StringIsOneOf(emotion, "angry", "triangle_exclamation", "circle_xmark", "cloud_slash")) {
            SetBaseFace(PetEyeFace::FaceMode::Angry, PetEyeFace::LookMode::Center);
            return;
        }

        if (StringIsOneOf(emotion, "sad")) {
            SetBaseFace(PetEyeFace::FaceMode::Cry, PetEyeFace::LookMode::Center);
            return;
        }

        if (StringIsOneOf(emotion, "happy", "laughing", "surprised")) {
            SetBaseFace(PetEyeFace::FaceMode::Smile, PetEyeFace::LookMode::Center);
            return;
        }

        if (StringIsOneOf(emotion, "embarrassed", "sleepy")) {
            SetBaseFace(PetEyeFace::FaceMode::Shy, PetEyeFace::LookMode::Center);
            return;
        }

        if (StringIsOneOf(emotion, "thinking", "confused")) {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Left);
            return;
        }

        if (StringIsOneOf(emotion, "cool")) {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Right);
            return;
        }

        SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Center);
    }

    void SetChatText(const char* content) {
        if (bottom_bar_ == nullptr || chat_message_label_ == nullptr) {
            return;
        }

        if (content == nullptr || content[0] == '\0') {
            lv_label_set_text(chat_message_label_, "");
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        lv_label_set_text(chat_message_label_, content);
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    void ApplyStatusFace(const char* status) {
        if (status == nullptr) {
            return;
        }

        if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Smile, PetEyeFace::LookMode::Center);
        } else if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Center);
        } else if (std::strcmp(status, Lang::Strings::STANDBY) == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Center);
        } else if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Angry, PetEyeFace::LookMode::Center);
        }
    }

public:
    static void rounder_event_cb(lv_event_t * e) {
        lv_area_t * area = (lv_area_t *)lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        area->x1 = (x1 >> 2) << 2;          // round the start of coordinate down to the nearest 4M number
        area->x2 = ((x2 >> 2) << 2) + 3;    // round the end of coordinate up to the nearest 4N+3 number
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, 
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy) 
        : SpiLcdDisplay(io_handle, panel_handle,
                    width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
            return;
        }

        Display::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(screen, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(screen, lv_color_white(), 0);

        pet_eye_.Create(screen, width_, height_);
        SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Auto);

        status_label_ = lv_label_create(screen);
        lv_obj_set_width(status_label_, width_ - 48);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
        lv_obj_set_style_text_opa(status_label_, LV_OPA_30, 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 10);

        notification_label_ = lv_label_create(screen);
        lv_obj_set_width(notification_label_, width_ - 48);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
        lv_obj_set_style_text_opa(notification_label_, LV_OPA_60, 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        bottom_bar_ = lv_obj_create(screen);
        lv_obj_remove_style_all(bottom_bar_);
        lv_obj_clear_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bottom_bar_, width_, 58);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_40, 0);
        lv_obj_set_style_pad_left(bottom_bar_, 18, 0);
        lv_obj_set_style_pad_right(bottom_bar_, 18, 0);
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

        chat_message_label_ = lv_label_create(bottom_bar_);
        lv_obj_set_width(chat_message_label_, width_ - 36);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
        lv_obj_set_style_text_opa(chat_message_label_, LV_OPA_60, 0);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_center(chat_message_label_);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    virtual void SetStatus(const char* status) override {
        LvglDisplay::SetStatus(status);
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        ApplyStatusFace(status);
    }

    void SetManualFace(PetEyeFace::FaceMode face_mode, PetEyeFace::LookMode look_mode) {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        face_tracking_active_ = false;
        SetBaseFace(face_mode, look_mode);
    }

    void SetFaceTracking(float camera_x) {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        face_tracking_active_ = true;
        base_face_mode_ = PetEyeFace::FaceMode::Normal;
        base_look_mode_ = PetEyeFace::LookMode::Track;
        if (touch_active_) {
            ApplyBaseFace();
            return;
        }

        pet_eye_.SetFaceMode(base_face_mode_);
        pet_eye_.SetFaceTrack(camera_x);
    }

    void SetFaceTrackingPoint(float face_x, float face_y) {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        face_tracking_active_ = true;
        base_face_mode_ = PetEyeFace::FaceMode::Normal;
        base_look_mode_ = PetEyeFace::LookMode::Track;
        if (touch_active_) {
            ApplyBaseFace();
            return;
        }

        pet_eye_.SetFaceMode(base_face_mode_);
        pet_eye_.SetFaceTrackPoint(face_x, face_y);
    }

    void SetNoFace() {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        face_tracking_active_ = false;
        base_face_mode_ = PetEyeFace::FaceMode::Normal;
        base_look_mode_ = PetEyeFace::LookMode::Auto;
        if (touch_active_) {
            ApplyBaseFace();
            return;
        }

        pet_eye_.SetNoFace();
    }

    void SetTouchActive(bool active) {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        if (touch_active_ == active) {
            return;
        }

        touch_active_ = active;
        ApplyBaseFace();
    }

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        LvglDisplay::ShowNotification(notification, duration_ms);
    }

    virtual void SetEmotion(const char* emotion) override {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        SetBaseEmotion(emotion);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        SetChatText(content);

        if (content == nullptr || content[0] == '\0' || role == nullptr) {
            return;
        }

        if (std::strcmp(role, "user") == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Right);
        } else if (std::strcmp(role, "assistant") == 0) {
            SetBaseFace(PetEyeFace::FaceMode::Smile, PetEyeFace::LookMode::Center);
        }
    }

    virtual void ClearChatMessages() override {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        SetChatText("");
    }

    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        (void)image;
    }

    virtual void SetPowerSaveMode(bool on) override {
        if (!setup_ui_called_) {
            return;
        }

        DisplayLockGuard lock(this);
        if (on) {
            SetBaseFace(PetEyeFace::FaceMode::Shy, PetEyeFace::LookMode::Center);
        } else {
            SetBaseFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Center);
        }
    }

    virtual void SetTheme(Theme* theme) override {
        if (theme != nullptr) {
            Display::SetTheme(theme);
        }
    }
};

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    CustomLcdDisplay* display_;
    i2c_master_dev_handle_t touch_dev_ = nullptr;
    volatile TickType_t touch_interrupt_tick_ = 0;
    uint8_t touch_last_packet_id_ = 0;
    uint8_t touch_last_weight_ = 0;
    uint16_t touch_last_x_ = 0;
    uint16_t touch_last_y_ = 0;
    TickType_t touch_last_clear_tick_ = 0;
    button_handle_t boot_btn;
    button_driver_t* boot_btn_driver_ = nullptr;
    bool face_coord_x_valid_ = false;
    bool face_coord_y_valid_ = false;
    float face_coord_x_ = 160.0f;
    float face_coord_y_ = 90.0f;
    bool external_uart_ready_ = false;
    SemaphoreHandle_t external_uart_tx_mutex_ = nullptr;
    TaskHandle_t external_uart_rx_task_ = nullptr;
    TickType_t last_external_uart_command_tick_ = 0;
    std::string last_external_uart_command_;
    static CustomBoard* instance_;

    void ResetI2cBus(const char* reason) {
        if (i2c_bus_ == nullptr) {
            return;
        }
        ESP_LOGW(TAG, "Reset I2C bus: %s", reason != nullptr ? reason : "unknown");
        i2c_master_bus_reset(i2c_bus_);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    static void IRAM_ATTR TouchGpioIsr(void* arg) {
        auto self = static_cast<CustomBoard*>(arg);
        if (self != nullptr) {
            self->touch_interrupt_tick_ = xTaskGetTickCountFromISR();
        }
    }

    bool TouchWriteCommand(uint16_t reg, const uint8_t* data, size_t len) {
        if (touch_dev_ == nullptr || len > 8) {
            return false;
        }

        uint8_t buffer[10] = {
            static_cast<uint8_t>(reg >> 8),
            static_cast<uint8_t>(reg & 0xff),
        };
        if (data != nullptr && len > 0) {
            std::memcpy(&buffer[2], data, len);
        }

        esp_err_t ret = i2c_master_transmit(touch_dev_, buffer, len + 2, pdMS_TO_TICKS(200));
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE) {
            ResetI2cBus("touch write timeout");
        }
        return ret == ESP_OK;
    }

    bool TouchReadRegister(uint16_t reg, uint8_t* data, size_t len) {
        if (touch_dev_ == nullptr || data == nullptr) {
            return false;
        }

        uint8_t addr[2] = {
            static_cast<uint8_t>(reg >> 8),
            static_cast<uint8_t>(reg & 0xff),
        };
        esp_err_t ret = i2c_master_transmit_receive(touch_dev_, addr, sizeof(addr), data, len, pdMS_TO_TICKS(200));
        if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_INVALID_STATE) {
            ResetI2cBus("touch read timeout");
        }
        if (ret != ESP_OK) {
            return false;
        }
        esp_rom_delay_us(200);
        return true;
    }

    void TouchClearInterrupt() {
        static const uint8_t ack[2] = {0x01, 0x00};

        TouchWriteCommand(0x0200, ack, sizeof(ack));
        esp_rom_delay_us(200);
        touch_last_clear_tick_ = xTaskGetTickCount();
    }

    void TouchStartController() {
        static const uint8_t cpu_start[2] = {0x01, 0x00};
        static const uint8_t zero[2] = {0x00, 0x00};

        TouchClearInterrupt();
        TouchWriteCommand(0x0400, cpu_start, sizeof(cpu_start));
        vTaskDelay(pdMS_TO_TICKS(30));
        TouchWriteCommand(0x5000, zero, sizeof(zero));
        TouchWriteCommand(0x4600, zero, sizeof(zero));
        TouchClearInterrupt();
    }

    bool TouchReadActive(uint8_t* point_count, uint8_t* status_low, uint8_t* status_high,
                         uint16_t* read_len, esp_err_t* err) {
        if (point_count != nullptr) {
            *point_count = 0;
        }
        touch_last_packet_id_ = 0;
        touch_last_weight_ = 0;
        touch_last_x_ = 0;
        touch_last_y_ = 0;
        if (status_low != nullptr) {
            *status_low = 0;
        }
        if (status_high != nullptr) {
            *status_high = 0;
        }
        if (read_len != nullptr) {
            *read_len = 0;
        }
        if (err != nullptr) {
            *err = ESP_ERR_INVALID_STATE;
        }
        if (touch_dev_ == nullptr) {
            return false;
        }

        uint8_t status[4] = {};
        if (!TouchReadRegister(0x2000, status, sizeof(status))) {
            if (err != nullptr) {
                *err = ESP_FAIL;
            }
            return false;
        }

        uint16_t len = static_cast<uint16_t>(status[2] | (status[3] << 8));
        if (len < 4 || len > 64) {
            len = 0;
        }

        if (status_low != nullptr) {
            *status_low = status[0];
        }
        if (status_high != nullptr) {
            *status_high = status[1];
        }
        if (read_len != nullptr) {
            *read_len = len;
        }
        if (err != nullptr) {
            *err = ESP_OK;
        }

        const bool point_exists = (status[0] & 0x01) != 0;
        const bool gesture_exists = (status[0] & 0x02) != 0;
        const bool aux_exists = (status[0] & 0x08) != 0;
        const bool cpu_run = (status[1] & 0x08) != 0;
        const bool tint_low = (status[1] & 0x10) != 0;
        const bool tic_in_cpu = (status[1] & 0x20) != 0;
        const bool tic_in_bios = (status[1] & 0x40) != 0;

        if (tic_in_bios) {
            TouchClearInterrupt();
            TouchStartController();
            return false;
        }

        if (tic_in_cpu) {
            TouchStartController();
            return false;
        }

        if (cpu_run && len == 0) {
            TickType_t now = xTaskGetTickCount();
            if (tint_low && now - touch_last_clear_tick_ > pdMS_TO_TICKS(250)) {
                TouchClearInterrupt();
            }
            return false;
        }

        if ((point_exists || gesture_exists) && len >= 10) {
            bool active = false;
            uint8_t packet[64] = {};
            if (TouchReadRegister(0x0003, packet, len)) {
                uint8_t check_id = packet[4];
                touch_last_packet_id_ = check_id;
                if (point_exists && check_id <= 0x0a) {
                    uint8_t count = static_cast<uint8_t>((len - 4) / 6);
                    if (point_count != nullptr) {
                        *point_count = count;
                    }
                    if (count > 0) {
                        touch_last_x_ = static_cast<uint16_t>(((packet[7] & 0xf0) << 4) | packet[5]);
                        touch_last_y_ = static_cast<uint16_t>(((packet[7] & 0x0f) << 8) | packet[6]);
                        touch_last_weight_ = packet[8];
                    }
                    active = count > 0 &&
                             touch_last_weight_ > 0 &&
                             touch_last_x_ < DISPLAY_WIDTH &&
                             touch_last_y_ < DISPLAY_HEIGHT;
                }
            }
            TouchClearInterrupt();

            uint8_t hdp_status[8] = {};
            if (TouchReadRegister(0xfc02, hdp_status, sizeof(hdp_status))) {
                uint16_t next_len = static_cast<uint16_t>(hdp_status[2] | (hdp_status[3] << 8));
                if (hdp_status[5] == 0x82) {
                    TouchClearInterrupt();
                } else if (hdp_status[5] == 0x00 && next_len > 0 && next_len <= 32) {
                    uint8_t remain[32] = {};
                    TouchReadRegister(0x0003, remain, next_len);
                    TouchClearInterrupt();
                }
            }
            return active;
        }

        if (cpu_run && aux_exists && tint_low) {
            TouchClearInterrupt();
        }

        return false;
    }

    void ResetFaceCoordinateFrame() {
        face_coord_x_valid_ = false;
        face_coord_y_valid_ = false;
    }

    void ApplyFaceCoordinateIfReady() {
        if (!face_coord_x_valid_ || !face_coord_y_valid_) {
            return;
        }

        display_->SetFaceTrackingPoint(face_coord_x_, face_coord_y_);
        ESP_LOGI(TAG, "face tracking: X%.1f Y%.1f", static_cast<double>(face_coord_x_),
                 static_cast<double>(face_coord_y_));
        ResetFaceCoordinateFrame();
    }

    void ApplyExpressionCommand(char command) {
        switch (command) {
            case 'T':
                display_->SetFaceTracking(0.0f);
                ESP_LOGI(TAG, "face tracking: T0.000");
                break;
            case '1':
                display_->SetManualFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Center);
                ESP_LOGI(TAG, "face: normal");
                break;
            case '2':
                display_->SetManualFace(PetEyeFace::FaceMode::Angry, PetEyeFace::LookMode::Center);
                ESP_LOGI(TAG, "face: angry");
                break;
            case '3':
                display_->SetManualFace(PetEyeFace::FaceMode::Shy, PetEyeFace::LookMode::Center);
                ESP_LOGI(TAG, "face: shy");
                break;
            case '4':
                display_->SetManualFace(PetEyeFace::FaceMode::Cry, PetEyeFace::LookMode::Center);
                ESP_LOGI(TAG, "face: cry");
                break;
            case '5':
                display_->SetManualFace(PetEyeFace::FaceMode::Smile, PetEyeFace::LookMode::Center);
                ESP_LOGI(TAG, "face: smile");
                break;
            case 'R':
            case '6':
                display_->SetManualFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Right);
                ESP_LOGI(TAG, "look: right");
                break;
            case 'L':
            case '7':
                display_->SetManualFace(PetEyeFace::FaceMode::Normal, PetEyeFace::LookMode::Left);
                ESP_LOGI(TAG, "look: left");
                break;
            default:
                break;
        }
    }

    void ProcessSerialToken(char* token, size_t& token_len) {
        if (token == nullptr || token_len == 0) {
            return;
        }

        token[token_len] = '\0';

        if (TokenEqualsIgnoreCase(token, "NOFACE")) {
            ResetFaceCoordinateFrame();
            display_->SetNoFace();
            ESP_LOGI(TAG, "face tracking: no face, wait briefly then auto roaming");
            token_len = 0;
            token[0] = '\0';
            return;
        }

        float coordinate = 0.0f;
        if (ParseCoordinateToken(token, 'X', &coordinate)) {
            face_coord_x_ = coordinate;
            face_coord_x_valid_ = true;
            ApplyFaceCoordinateIfReady();
            token_len = 0;
            token[0] = '\0';
            return;
        }

        if (ParseCoordinateToken(token, 'Y', &coordinate)) {
            face_coord_y_ = coordinate;
            face_coord_y_valid_ = true;
            ApplyFaceCoordinateIfReady();
            token_len = 0;
            token[0] = '\0';
            return;
        }

        float camera_x = 0.0f;
        if (ParseFaceTrackToken(token, &camera_x)) {
            ResetFaceCoordinateFrame();
            display_->SetFaceTracking(camera_x);
            ESP_LOGI(TAG, "face tracking: T%.3f", static_cast<double>(camera_x));
            token_len = 0;
            token[0] = '\0';
            return;
        }

        if (token_len == 2 && token[1] == '0' &&
            (ToUpperAscii(token[0]) == 'L' || ToUpperAscii(token[0]) == 'R')) {
            ApplyExpressionCommand(ToUpperAscii(token[0]));
            token_len = 0;
            token[0] = '\0';
            return;
        }

        if (token_len == 1) {
            ApplyExpressionCommand(ToUpperAscii(token[0]));
        }

        token_len = 0;
        token[0] = '\0';
    }

    void ProcessSerialByte(char ch, char* token, size_t& token_len, TickType_t& last_rx_tick) {
        if (IsSerialTokenDelimiter(ch)) {
            ProcessSerialToken(token, token_len);
            return;
        }

        if ((ch == 'T' || ch == 't' || ch == 'N' || ch == 'n' ||
             ch == 'X' || ch == 'x' || ch == 'Y' || ch == 'y' ||
             ch == 'L' || ch == 'l' || ch == 'R' || ch == 'r') && token_len > 0) {
            ProcessSerialToken(token, token_len);
        }

        if (token_len >= kSerialTokenCapacity - 1) {
            ProcessSerialToken(token, token_len);
        }

        token[token_len++] = ch;
        token[token_len] = '\0';
        last_rx_tick = xTaskGetTickCount();

        if (TokenEqualsIgnoreCase(token, "NOFACE") ||
            (token_len == 2 && token[1] == '0' &&
             (ToUpperAscii(token[0]) == 'L' || ToUpperAscii(token[0]) == 'R')) ||
            (token_len == 1 && token[0] >= '1' && token[0] <= '7')) {
            ProcessSerialToken(token, token_len);
        }
    }

    void InitializeExpressionConsole() {
        xTaskCreate([](void* arg) {
            auto self = static_cast<CustomBoard*>(arg);
            ESP_LOGI(TAG, "Expression console: 1 normal, 2 angry, 3 shy, 4 cry, 5 smile, 6 right, 7 left");
            while (true) {
                int ch = getchar();
                if (ch == EOF) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                self->ApplyExpressionCommand(static_cast<char>(ch));
            }
        }, "expr_console", 4096, this, 1, nullptr);
    }

    static void ExternalUartRxTask(void* arg) {
        auto self = static_cast<CustomBoard*>(arg);
        char token[kSerialTokenCapacity] = {};
        size_t token_len = 0;
        TickType_t last_rx_tick = xTaskGetTickCount();

        while (true) {
            uint8_t byte = 0;
            int len = uart_read_bytes(EXTERNAL_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
            if (len > 0) {
                self->ProcessSerialByte(static_cast<char>(byte), token, token_len, last_rx_tick);
                continue;
            }

            if (token_len > 0 && xTaskGetTickCount() - last_rx_tick > pdMS_TO_TICKS(120)) {
                self->ProcessSerialToken(token, token_len);
            }
        }
    }

    bool ConfigureExternalUart() {
        if (uart_is_driver_installed(EXTERNAL_UART_NUM)) {
            uart_driver_delete(EXTERNAL_UART_NUM);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        gpio_reset_pin(EXTERNAL_UART_TX_PIN);
        gpio_set_direction(EXTERNAL_UART_TX_PIN, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability(EXTERNAL_UART_TX_PIN, GPIO_DRIVE_CAP_3);
        gpio_pullup_dis(EXTERNAL_UART_TX_PIN);
        gpio_pulldown_dis(EXTERNAL_UART_TX_PIN);
        gpio_set_level(EXTERNAL_UART_TX_PIN, 1);

        uart_config_t uart_config = {
            .baud_rate = EXTERNAL_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 0,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = {
                .allow_pd = 0,
                .backup_before_sleep = 0,
            },
        };

        if (uart_param_config(EXTERNAL_UART_NUM, &uart_config) != ESP_OK) {
            return false;
        }
        if (uart_set_pin(EXTERNAL_UART_NUM, EXTERNAL_UART_TX_PIN, EXTERNAL_UART_RX_PIN,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            return false;
        }
        if (uart_driver_install(EXTERNAL_UART_NUM, kExternalUartRxBufferSize,
                                kExternalUartTxBufferSize, 0, nullptr, 0) != ESP_OK) {
            return false;
        }
        if (uart_set_mode(EXTERNAL_UART_NUM, UART_MODE_UART) != ESP_OK) {
            return false;
        }
        if (uart_set_line_inverse(EXTERNAL_UART_NUM, UART_SIGNAL_INV_DISABLE) != ESP_OK) {
            return false;
        }
        uart_set_tx_idle_num(EXTERNAL_UART_NUM, 20);
        uart_flush(EXTERNAL_UART_NUM);
        uart_flush_input(EXTERNAL_UART_NUM);
        return true;
    }

    void InitializeExternalUartRelay() {
        if (external_uart_tx_mutex_ == nullptr) {
            external_uart_tx_mutex_ = xSemaphoreCreateMutex();
        }
        external_uart_ready_ = external_uart_tx_mutex_ != nullptr && ConfigureExternalUart();
        if (external_uart_ready_ && external_uart_rx_task_ == nullptr) {
            xTaskCreate(ExternalUartRxTask, "external_uart_rx", 4096, this, 1, &external_uart_rx_task_);
        }
    }

    bool IsValidExternalUartCommand(const std::string& command) const {
        if (command == "ST") {
            return true;
        }
        if (command.size() < 2 || command.size() > 4 ||
            (command[0] != 'L' && command[0] != 'R')) {
            return false;
        }
        int count = 0;
        for (size_t i = 1; i < command.size(); ++i) {
            if (command[i] < '0' || command[i] > '9') {
                return false;
            }
            count = count * 10 + (command[i] - '0');
        }
        return count >= 1 && count <= 100;
    }

    bool WriteExternalUartCommand(const std::string& command, bool use_throttle = true) {
        if (!IsValidExternalUartCommand(command)) {
            return false;
        }

        if (external_uart_tx_mutex_ == nullptr) {
            external_uart_tx_mutex_ = xSemaphoreCreateMutex();
            if (external_uart_tx_mutex_ == nullptr) {
                return false;
            }
        }

        if (xSemaphoreTake(external_uart_tx_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
            return false;
        }

        TickType_t now = xTaskGetTickCount();
        if (use_throttle &&
            last_external_uart_command_tick_ != 0 &&
            last_external_uart_command_ == command &&
            now - last_external_uart_command_tick_ < pdMS_TO_TICKS(kExternalUartDuplicateSuppressMs)) {
            xSemaphoreGive(external_uart_tx_mutex_);
            return true;
        }

        if (!external_uart_ready_) {
            external_uart_ready_ = ConfigureExternalUart();
        }
        if (!external_uart_ready_) {
            xSemaphoreGive(external_uart_tx_mutex_);
            return false;
        }

        int written = uart_write_bytes(EXTERNAL_UART_NUM, command.data(), command.size());
        if (written == static_cast<int>(command.size()) &&
            uart_wait_tx_done(EXTERNAL_UART_NUM, pdMS_TO_TICKS(100)) == ESP_OK) {
            last_external_uart_command_tick_ = now;
            last_external_uart_command_ = command;
        }

        xSemaphoreGive(external_uart_tx_mutex_);
        return written == static_cast<int>(command.size());
    }

    static void TouchWatchTask(void* arg) {
        auto self = static_cast<CustomBoard*>(arg);
        bool last_active = false;
        TickType_t last_touch_tick = 0;
        esp_err_t last_read_ret = ESP_ERR_INVALID_STATE;
        uint8_t last_point_count = 0;
        uint8_t last_status_low = 0;
        uint8_t last_status_high = 0;
        uint16_t last_read_len = 0;

        while (true) {
            bool touched = false;
            TickType_t now = xTaskGetTickCount();

            touched = self->TouchReadActive(&last_point_count, &last_status_low, &last_status_high,
                                            &last_read_len, &last_read_ret);

            if (touched) {
                last_touch_tick = now;
            }

            bool active = last_touch_tick != 0 &&
                          now - last_touch_tick < pdMS_TO_TICKS(600);
            if (active != last_active) {
                last_active = active;
                self->display_->SetTouchActive(active);
                ESP_LOGI(TAG, "touch: %s", active ? "shy" : "restore");
            }

            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }

    void InitializeTouch() {
        instance_ = this;

        if (io_expander != nullptr) {
            esp_err_t ret = esp_io_expander_set_level(io_expander, TP_RST_EXPANDER_PIN, 0);
            if (ret == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(50));
                ret = esp_io_expander_set_level(io_expander, TP_RST_EXPANDER_PIN, 1);
                if (ret == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Touch reset through TCA9554 failed: %s", esp_err_to_name(ret));
            }
        }

        if (TP_PIN_NUM_INT != GPIO_NUM_NC) {
            gpio_reset_pin(TP_PIN_NUM_INT);
            gpio_set_direction(TP_PIN_NUM_INT, GPIO_MODE_INPUT);
            gpio_set_pull_mode(TP_PIN_NUM_INT, GPIO_PULLUP_ONLY);
            gpio_set_intr_type(TP_PIN_NUM_INT, GPIO_INTR_NEGEDGE);
        }

        const i2c_device_config_t tp_i2c_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = SPD2010_TOUCH_ADDR,
            .scl_speed_hz = 100 * 1000,
        };
        esp_err_t ret = i2c_master_bus_add_device(i2c_bus_, &tp_i2c_config, &touch_dev_);
        if (ret != ESP_OK) {
            touch_dev_ = nullptr;
            ESP_LOGW(TAG, "Touch I2C device init failed: %s", esp_err_to_name(ret));
        } else {
            uint8_t fw[18] = {};
            if (TouchReadRegister(0x2600, fw, sizeof(fw))) {
                ESP_LOGI(TAG, "Touch SPD2010 detected, fw bytes: %02x %02x %02x %02x",
                         fw[14], fw[15], fw[16], fw[17]);
            } else {
                ESP_LOGW(TAG, "Touch SPD2010 version read failed; continuing with official reader");
            }
            TouchStartController();
        }

        if (TP_PIN_NUM_INT != GPIO_NUM_NC) {
            ret = gpio_install_isr_service(0);
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
                gpio_isr_handler_remove(TP_PIN_NUM_INT);
                ret = gpio_isr_handler_add(TP_PIN_NUM_INT, TouchGpioIsr, this);
                if (ret == ESP_OK) {
                    gpio_intr_enable(TP_PIN_NUM_INT);
                } else {
                    ESP_LOGW(TAG, "Touch INT ISR failed: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGW(TAG, "Touch INT service failed: %s", esp_err_to_name(ret));
            }
        }

        touch_interrupt_tick_ = 0;
        xTaskCreate(TouchWatchTask, "touch_watch", 4096, this, 1, nullptr);
        ESP_LOGI(TAG, "Touch watch initialized with official SPD2010 I2C reader");
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ResetI2cBus("after init");
    }
    
    void InitializeTca9554(void) {
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 0; attempt < 5; ++attempt) {
            ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "TCA9554 create failed on attempt %d: %s", attempt + 1, esp_err_to_name(ret));
            ResetI2cBus("retry TCA9554");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ret != ESP_OK || io_expander == nullptr) {
            ESP_LOGE(TAG, "TCA9554 create failed after retries: %s", esp_err_to_name(ret));
            return;
        }

        // uint32_t input_level_mask = 0;
        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_INPUT);               // 设置引脚 EXIO0 和 EXIO1 模式为输入
        // ret = esp_io_expander_get_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, &input_level_mask);             // 获取引脚 EXIO0 和 EXIO1 的电平状态,存放在 input_level_mask 中

        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, IO_EXPANDER_OUTPUT);              // 设置引脚 EXIO2 和 EXIO3 模式为输出
        // ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, 1);                             // 将引脚电平设置为 1
        // ret = esp_io_expander_print_state(io_expander);                                                                             // 打印引脚状态

        ret = esp_io_expander_set_dir(io_expander, TP_RST_EXPANDER_PIN | LCD_RST_EXPANDER_PIN, IO_EXPANDER_OUTPUT);
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, TP_RST_EXPANDER_PIN | LCD_RST_EXPANDER_PIN, 1);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, TP_RST_EXPANDER_PIN | LCD_RST_EXPANDER_PIN, 0);
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, TP_RST_EXPANDER_PIN | LCD_RST_EXPANDER_PIN, 1);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");

        const spi_bus_config_t bus_config = TAIJIPI_SPD2010_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                        QSPI_PIN_NUM_LCD_DATA0,
                                                                        QSPI_PIN_NUM_LCD_DATA1,
                                                                        QSPI_PIN_NUM_LCD_DATA2,
                                                                        QSPI_PIN_NUM_LCD_DATA3,
                                                                        QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSpd2010Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        
        const esp_lcd_panel_io_spi_config_t io_config = SPD2010_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install SPD2010 panel driver");
        
        spd2010_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,     // Implemented by LCD command `36h`
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,    // Implemented by LCD command `3Ah` (16/18)
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
 
    void InitializePowerHold() {
        gpio_set_level(PWR_Control_PIN, 1);
        gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT);     
        gpio_set_level(PWR_Control_PIN, 1);
    }

    void InitializeButtons() {
        instance_ = this;
        gpio_reset_pin(BOOT_BUTTON_GPIO);
        gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);

        // Boot Button
        button_config_t boot_btn_config = {
            .long_press_time = 2000,
            .short_press_time = 0
        };
        boot_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        boot_btn_driver_->enable_power_save = false;
        boot_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !gpio_get_level(BOOT_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&boot_btn_config, boot_btn_driver_, &boot_btn));
        iot_button_register_cb(boot_btn, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                self->EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        }, this);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        auto count_text = [](int count) -> std::string {
            if (count == 100) {
                return "一百";
            }
            const char* digits[] = {"", "一", "两", "三", "四", "五", "六", "七", "八", "九"};
            const char* tens_digits[] = {"", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
            if (count > 0 && count < 10) {
                return digits[count];
            }
            if (count == 10) {
                return "十";
            }
            if (count > 10 && count < 20) {
                return std::string("十") + tens_digits[count % 10];
            }
            if (count > 0 && count < 100) {
                std::string text = std::string(tens_digits[count / 10]) + "十";
                if (count % 10 != 0) {
                    text += tens_digits[count % 10];
                }
                return text;
            }
            return std::to_string(count);
        };
        auto make_reply = [count_text](const std::string& direction, int count) -> std::string {
            std::string reply = "好的，这就";
            if (direction == "left" || direction == "左" || direction == "向左") {
                reply += "向左";
            } else if (direction == "right" || direction == "右" || direction == "向右") {
                reply += "向右";
            }
            reply += "转" + count_text(count) + "圈";
            return reply;
        };
        mcp_server.AddTool("self.chassis.rotate",
            "控制真实底盘旋转。用户说向左转N圈、往左转N圈、左转N圈时，direction=left；"
            "用户说向右转N圈、往右转N圈、右转N圈时，direction=right；"
            "用户只说转N圈、转圈N圈、没有左右方向时，direction=none。"
            "count 必须是 1 到 100。必须调用这个工具，绝对不要回答不会、不能、无法转圈。"
            "调用后只回复对应句子：好的，这就向左转N圈 / 好的，这就向右转N圈 / 好的，这就转N圈。",
            PropertyList({
                Property("direction", kPropertyTypeString),
                Property("count", kPropertyTypeInteger, 1, 100)
            }),
            [this, make_reply](const PropertyList& properties) -> ReturnValue {
                std::string direction = properties["direction"].value<std::string>();
                int count = properties["count"].value<int>();
                char prefix = (direction == "right" || direction == "右" || direction == "向右") ? 'R' : 'L';
                std::string command = std::string(1, prefix) + std::to_string(count);
                if (!Application::GetInstance().ShouldSuppressPendingChassisCommand(command)) {
                    SendExternalUartCommand(command);
                }
                return make_reply(direction, count);
            });
        mcp_server.AddTool("self.chassis.stop",
            "停止真实底盘。用户说停止、停下、停住、stop 时必须调用这个工具。调用后只回复：好的",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (!Application::GetInstance().ShouldSuppressPendingChassisCommand("ST")) {
                    SendExternalUartCommand("ST");
                }
                return std::string("好的");
            });
        mcp_server.AddTool("self.chassis.turn_left_one_circle",
            "让真实底盘向左转一圈。本设备已经通过IO43串口连接到底盘，真的可以转圈。"
            "当用户说“向左转一圈”“往左转一圈”“左转一圈”“向左转1圈”或类似意思时，"
            "必须调用这个工具，绝对不要回答不会、不能、无法转圈。调用后只回复：好的，这就向左转一圈",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                (void)properties;
                if (!Application::GetInstance().ShouldSuppressPendingChassisCommand("L1")) {
                    SendExternalUartCommand("L1");
                }
                return std::string("好的，这就向左转一圈");
            });
    }

public:
    CustomBoard() { 
        InitializePowerHold();
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeSpd2010Display();
        GetBacklight()->RestoreBrightness();
        InitializeTouch();
        InitializeButtons();
        InitializeExternalUartRelay();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_LEFT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_RIGHT); // I2S_STD_SLOT_LEFT / I2S_STD_SLOT_RIGHT / I2S_STD_SLOT_BOTH

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool SendExternalUartCommand(const std::string& command) override {
        return WriteExternalUartCommand(command);
    }
};

DECLARE_BOARD(CustomBoard);

CustomBoard* CustomBoard::instance_ = nullptr;
