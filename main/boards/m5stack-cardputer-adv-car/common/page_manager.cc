#include "page_manager.h"

#include "application.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "PageManager"

namespace {

size_t FreeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

bool PanelVisible(lv_obj_t* panel) {
    return panel != nullptr && !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void ChatPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display != nullptr) {
        display->ShowChatUi();
    }
    // Re-sync after Radio/Music steal the codec. Safe on boot too (wake word
    // Enable* no-ops until models are ready).
    Application::GetInstance().RestoreAudioRouting();
}

void ChatPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    (void)display;
}

void PageManager::Initialize(CardputerAdvCarLcdDisplay* display, EmqxCarMqtt* mqtt) {
    display_ = display;
    mqtt_ = mqtt;
    car_page_.SetMqtt(mqtt);
    spider_page_.SetMqtt(mqtt);
    launcher_page_.SetNavigateCallback([this](PageId id) {
        // Defer off keyboard task (same as Fn page switch).
        Application::GetInstance().Schedule([this, id]() { ShowPage(id); });
    });
    current_ = PageId::Chat;
    chat_page_.OnEnter(display_);
}

Page* PageManager::GetPage(PageId id) {
    switch (id) {
        case PageId::Chat:
            return &chat_page_;
        case PageId::Car:
            return &car_page_;
        case PageId::Spider:
            return &spider_page_;
        case PageId::MjAc:
            return &mj_ac_page_;
        case PageId::Launcher:
            return &launcher_page_;
        case PageId::Clock:
            return &clock_page_;
        case PageId::Matrix:
            return &matrix_page_;
        case PageId::Music:
            return &cursor_page_;
        case PageId::Radio:
            return &radio_page_;
        default:
            return nullptr;
    }
}

void PageManager::RecoverToChat(const char* reason) {
    ESP_LOGE(TAG, "RecoverToChat: %s (was page %d) heap=%u", reason, static_cast<int>(current_),
             static_cast<unsigned>(FreeHeap()));
    Page* stuck = GetPage(current_);
    if (stuck != nullptr && current_ != PageId::Chat) {
        // OnLeave first (Music restores mic/AFE; Radio stops stream; hide failed panel).
        stuck->OnLeave(display_);
    }
    current_ = PageId::Chat;
    chat_page_.OnEnter(display_);
    if (display_ != nullptr && !display_->IsChatUiVisible()) {
        ESP_LOGE(TAG, "Chat UI still hidden after recover; trying Launcher heap=%u",
                 static_cast<unsigned>(FreeHeap()));
        chat_page_.OnLeave(display_);
        current_ = PageId::Launcher;
        launcher_page_.OnEnter(display_);
        if (!PanelVisible(launcher_page_.GetRootPanel())) {
            ESP_LOGE(TAG, "Launcher also failed; force ShowChatUi");
            current_ = PageId::Chat;
            if (display_ != nullptr) {
                display_->ShowChatUi();
            }
        }
    }
}

void PageManager::ShowPage(PageId id) {
    if (display_ == nullptr || id == current_) {
        return;
    }
    if (switching_) {
        ESP_LOGW(TAG, "ShowPage re-entry ignored %d -> %d", static_cast<int>(current_),
                 static_cast<int>(id));
        return;
    }

    Page* next = GetPage(id);
    Page* current = GetPage(current_);
    if (next == nullptr || current == nullptr) {
        return;
    }

    switching_ = true;
    const PageId prev = current_;
    ESP_LOGI(TAG, "ShowPage enter %d -> %d heap=%u", static_cast<int>(prev), static_cast<int>(id),
             static_cast<unsigned>(FreeHeap()));

    if (id == PageId::Chat && mqtt_ != nullptr && mqtt_->run() != 0) {
        mqtt_->PublishCarCmd(0, mqtt_->speed());
    }

    // Leave-first for exclusive→exclusive (IceBox IR / Radio / …). Chat→page
    // still enters first so HideChatUi never leaves a WiFi-only blank frame.
    if (prev != PageId::Chat) {
        current->OnLeave(display_);
        current_ = id;
        next->OnEnter(display_);
    } else {
        current_ = id;
        next->OnEnter(display_);
        current->OnLeave(display_);
    }

    if (id == PageId::Chat) {
        if (!display_->IsChatUiVisible()) {
            RecoverToChat("chat UI still hidden after switch to Chat");
            switching_ = false;
            ESP_LOGI(TAG, "ShowPage leave (recovered) heap=%u", static_cast<unsigned>(FreeHeap()));
            return;
        }
    } else {
        lv_obj_t* panel = next->GetRootPanel();
        if (!PanelVisible(panel)) {
            RecoverToChat("next page panel null/hidden after OnEnter");
            switching_ = false;
            ESP_LOGI(TAG, "ShowPage leave (recovered) heap=%u", static_cast<unsigned>(FreeHeap()));
            return;
        }
        // Keep exclusive panel above any chat chrome that UpdateStatusBar may touch.
        DisplayLockGuard lock(display_);
        lv_obj_move_foreground(panel);
    }

    ESP_LOGI(TAG, "ShowPage leave %d -> %d ok heap=%u", static_cast<int>(prev),
             static_cast<int>(current_), static_cast<unsigned>(FreeHeap()));
    switching_ = false;
}

void PageManager::RefreshCurrentPage() {
    if (display_ == nullptr || switching_) {
        return;
    }
    Page* page = GetPage(current_);
    if (page != nullptr) {
        page->OnEnter(display_);
    }
}

bool PageManager::IsVehiclePage() const {
    return current_ == PageId::Car || current_ == PageId::Spider;
}

bool PageManager::IsAcPage() const {
    return current_ == PageId::MjAc;
}

bool PageManager::IsLauncherPage() const {
    return current_ == PageId::Launcher;
}

bool PageManager::IsExclusivePage() const {
    return current_ != PageId::Chat;
}

void PageManager::Tick() {
    if (switching_) {
        return;
    }
    // Soft watchdog: exclusive page must keep a visible panel.
    if (current_ != PageId::Chat) {
        Page* page = GetPage(current_);
        if (page != nullptr && !PanelVisible(page->GetRootPanel())) {
            RecoverToChat("tick watchdog: panel missing");
            return;
        }
    }

    if (current_ == PageId::Car) {
        car_page_.Tick(display_);
    } else if (current_ == PageId::Spider) {
        spider_page_.Tick(display_);
    } else if (current_ == PageId::MjAc) {
        mj_ac_page_.Tick(display_);
    } else if (current_ == PageId::Clock) {
        clock_page_.Tick(display_);
    } else if (current_ == PageId::Matrix) {
        matrix_page_.Tick(display_);
    } else if (current_ == PageId::Music) {
        cursor_page_.Tick(display_);
    } else if (current_ == PageId::Radio) {
        radio_page_.Tick(display_);
    } else if (current_ == PageId::Launcher) {
        launcher_page_.Tick(display_);
    }
}

bool PageManager::HandleVehicleKey(const KeyEvent& event) {
    if (current_ == PageId::Car) {
        return car_page_.HandleKey(event);
    }
    if (current_ == PageId::Spider) {
        return spider_page_.HandleKey(event);
    }
    if (current_ == PageId::MjAc) {
        return mj_ac_page_.HandleKey(event);
    }
    if (current_ == PageId::Launcher) {
        return launcher_page_.HandleKey(event);
    }
    // Clock / Matrix / Music / Radio: page-owned keys (Fn handled upstream).
    if (current_ == PageId::Clock || current_ == PageId::Matrix) {
        return true;
    }
    if (current_ == PageId::Music) {
        return true;
    }
    if (current_ == PageId::Radio) {
        return radio_page_.HandleKey(event);
    }
    return false;
}

bool PageManager::HandleVehicleLegacyKey(LegacyKeyCode key) {
    if (!IsVehiclePage()) {
        return false;
    }

    KeyEvent event{};
    event.pressed = true;
    switch (key) {
        case KEY_UP:
            event.key_code = KC_SEMICOLON;
            break;
        case KEY_DOWN:
            event.key_code = KC_DOT;
            break;
        case KEY_LEFT:
            event.key_code = KC_COMMA;
            break;
        case KEY_RIGHT:
            event.key_code = KC_SLASH;
            break;
        default:
            return false;
    }
    return HandleVehicleKey(event);
}
