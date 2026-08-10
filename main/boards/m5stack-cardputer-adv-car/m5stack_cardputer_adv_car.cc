#include "wifi_board.h"
#include "wifi_config_ui.h"
#include "codecs/es8311_audio_codec.h"
#include "cardputer_adv_lcd_display.h"
#include "emqx_mqtt_client.h"
#include "page_manager.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "tca8418_keyboard.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/i2s_common.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <algorithm>
#include <memory>

#define TAG "CardputerAdvCar"

// Backlight uses percentage scale (0-100). Keep a minimum of 30% to avoid a too-dim screen.
#define MIN_BRIGHTNESS 30

class M5StackCardputerAdvCarBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    CardputerAdvCarLcdDisplay* display_;
    Button boot_button_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Tca8418Keyboard* keyboard_ = nullptr;
    std::unique_ptr<WifiConfigUI> wifi_config_ui_;
    PageManager page_manager_;
    EmqxCarMqtt emqx_mqtt_;
    esp_timer_handle_t car_timer_ = nullptr;
    bool wifi_config_mode_ = false;

    static void OnCarTimer(void* arg) {
        auto* board = static_cast<M5StackCardputerAdvCarBoard*>(arg);
        board->emqx_mqtt_.Tick();
        board->page_manager_.Tick();
    }

    void InitializeCarTimer() {
        esp_timer_create_args_t args = {
            .callback = OnCarTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "car_mqtt_tick",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &car_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(car_timer_, 200000));
    }

    void InitializeI2c() {
        ESP_LOGI(TAG, "Initialize I2C bus");
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        ESP_LOGI(TAG, "I2C device scan:");
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        ESP_LOGI(TAG, "Initialize ST7789V2 display");

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.flags.sio_mode = 1;  // 3-wire SPI mode (M5GFX uses spi_3wire = true)
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7789 panel driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new CardputerAdvCarLcdDisplay(panel_io_, panel_,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetSetupUiCallback([this]() {
            emqx_mqtt_.Initialize(GetNetwork());
            page_manager_.Initialize(display_, &emqx_mqtt_);
            InitializeCarTimer();
        });
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeKeyboard() {
        ESP_LOGI(TAG, "Initialize TCA8418 keyboard");
        keyboard_ = new Tca8418Keyboard(i2c_bus_, KEYBOARD_TCA8418_ADDR, KEYBOARD_INT_PIN);
        keyboard_->Initialize();

        keyboard_->SetKeyCallback([this](LegacyKeyCode key) {
            HandleLegacyKeyPress(key);
        });

        keyboard_->SetKeyEventCallback([this](const KeyEvent& event) {
            HandleKeyEvent(event);
        });
    }

    bool HandlePageSwitchKey(const KeyEvent& event) {
        if (!event.pressed || !keyboard_->IsFnPressed()) {
            return false;
        }

        if (event.key_code == KC_1) {
            page_manager_.ShowPage(PageId::Chat);
            return true;
        }
        if (event.key_code == KC_2) {
            page_manager_.ShowPage(PageId::Car);
            return true;
        }
        if (event.key_code == KC_3) {
            page_manager_.ShowPage(PageId::Spider);
            return true;
        }
        return false;
    }

    void HandleKeyEvent(const KeyEvent& event) {
        if (HandlePageSwitchKey(event)) {
            return;
        }

        if (page_manager_.IsVehiclePage()) {
            if (page_manager_.HandleVehicleKey(event)) {
                return;
            }
            return;
        }

        if (wifi_config_mode_ && wifi_config_ui_) {
            auto result = wifi_config_ui_->HandleKeyEvent(event);
            if (result == WifiConfigResult::Connected) {
                ESP_LOGI(TAG, "WiFi connected via keyboard config");
                ExitWifiConfigMode();
            } else if (result == WifiConfigResult::Cancelled) {
                ESP_LOGI(TAG, "WiFi config cancelled");
                ExitWifiConfigMode();
            }
            return;
        }

        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring && event.pressed) {
            if (event.key_code == KC_W) {
                ESP_LOGI(TAG, "W key pressed - entering keyboard WiFi config");
                StartKeyboardWifiConfig();
            } else if (event.key_code == KC_S) {
                ESP_LOGI(TAG, "S key pressed - showing saved WiFi list");
                StartKeyboardWifiConfigSaved();
            }
        }
    }

    void HandleLegacyKeyPress(LegacyKeyCode key) {
        if (wifi_config_mode_) {
            return;
        }

        if (page_manager_.IsVehiclePage()) {
            page_manager_.HandleVehicleLegacyKey(key);
            return;
        }

        if (page_manager_.current_page() != PageId::Chat) {
            return;
        }

        auto& app = Application::GetInstance();
        auto* codec = GetAudioCodec();
        auto* backlight = GetBacklight();

        switch (key) {
            case KEY_UP: {
                int current_vol = codec->output_volume();
                int step = (current_vol <= 20 || current_vol >= 80) ? 1 : 10;
                int new_vol = std::min(100, current_vol + step);
                codec->SetOutputVolume(new_vol);
                char msg[32];
                snprintf(msg, sizeof(msg), "Volume: %d%%", new_vol);
                display_->ShowNotification(msg, 1500);
                ESP_LOGI(TAG, "Volume up: %d%%", new_vol);
                break;
            }
            case KEY_DOWN: {
                int current_vol = codec->output_volume();
                int step = (current_vol <= 20 || current_vol >= 80) ? 1 : 10;
                int new_vol = std::max(0, current_vol - step);
                codec->SetOutputVolume(new_vol);
                char msg[32];
                snprintf(msg, sizeof(msg), "Volume: %d%%", new_vol);
                display_->ShowNotification(msg, 1500);
                ESP_LOGI(TAG, "Volume down: %d%%", new_vol);
                break;
            }
            case KEY_RIGHT: {
                uint8_t current_br = backlight->brightness();
                int step = (current_br <= (MIN_BRIGHTNESS + 20) || current_br >= 80) ? 1 : 10;
                int new_br = std::min(100, (int)current_br + step);
                backlight->SetBrightness(new_br, true);
                char msg[32];
                snprintf(msg, sizeof(msg), "Brightness: %d%%", new_br);
                display_->ShowNotification(msg, 1500);
                ESP_LOGI(TAG, "Brightness up: %d%%", new_br);
                break;
            }
            case KEY_LEFT: {
                uint8_t current_br = backlight->brightness();
                int step = (current_br <= (MIN_BRIGHTNESS + 20) || current_br >= 80) ? 1 : 10;
                int new_br = std::max((int)MIN_BRIGHTNESS, (int)current_br - step);
                backlight->SetBrightness(new_br, true);
                char msg[32];
                snprintf(msg, sizeof(msg), "Brightness: %d%%", new_br);
                display_->ShowNotification(msg, 1500);
                ESP_LOGI(TAG, "Brightness down: %d%%", new_br);
                break;
            }
            case KEY_ENTER: {
                if (app.GetDeviceState() != kDeviceStateStarting) {
                    app.ToggleChatState();
                    ESP_LOGI(TAG, "Enter key: Toggle chat state");
                }
                break;
            }
            default:
                break;
        }
    }

    void StartKeyboardWifiConfig() {
        ESP_LOGI(TAG, "Starting keyboard WiFi config UI");
        wifi_config_mode_ = true;
        wifi_config_ui_ = std::make_unique<WifiConfigUI>(display_);
        wifi_config_ui_->SetConnectCallback([this](const std::string& ssid, const std::string& password) {
            AttemptWifiConnection(ssid, password);
        });
        wifi_config_ui_->Start();
    }

    void StartKeyboardWifiConfigSaved() {
        ESP_LOGI(TAG, "Starting keyboard WiFi config UI (saved list)");
        wifi_config_mode_ = true;
        wifi_config_ui_ = std::make_unique<WifiConfigUI>(display_);
        wifi_config_ui_->SetConnectCallback([this](const std::string& ssid, const std::string& password) {
            AttemptWifiConnection(ssid, password);
        });
        wifi_config_ui_->StartWithSavedList();
    }

    void AttemptWifiConnection(const std::string& ssid, const std::string& password) {
        ESP_LOGI(TAG, "Attempting WiFi connection to: %s", ssid.c_str());

        auto& ssid_manager = SsidManager::GetInstance();
        ssid_manager.AddSsid(ssid, password);

        auto& wifi_manager = WifiManager::GetInstance();
        if (wifi_manager.IsConfigMode()) {
            wifi_manager.StopConfigAp();
        }

        wifi_manager.StartStation();

        bool connected = false;
        for (int i = 0; i < 100; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (wifi_manager.IsConnected()) {
                connected = true;
                break;
            }
        }

        if (wifi_config_ui_) {
            wifi_config_ui_->OnConnectResult(connected);
        }
    }

    void ExitWifiConfigMode() {
        ESP_LOGI(TAG, "Exiting keyboard WiFi config mode");
        wifi_config_mode_ = false;
        wifi_config_ui_.reset();

        // TODO: wifi_config_ui may call lv_obj_clean on the display tree; restore page UI here.
        page_manager_.RefreshCurrentPage();

        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            TryWifiConnect();
        }
    }

public:
    M5StackCardputerAdvCarBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        I2cDetect();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeKeyboard();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static struct CardputerAdvEs8311 : public Es8311AudioCodec {
            CardputerAdvEs8311(void* i2c, i2c_port_t port, int in_rate, int out_rate,
                gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                gpio_num_t dout, gpio_num_t din, gpio_num_t pa,
                uint8_t addr, bool use_mclk)
                : Es8311AudioCodec(i2c, port, in_rate, out_rate,
                    mclk, bclk, ws, dout, din, pa, addr, use_mclk) {
                i2s_channel_disable(tx_handle_);
                i2s_channel_disable(rx_handle_);
            }
        } audio_codec(
            i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR,
            false);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT, 256);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCardputerAdvCarBoard);
