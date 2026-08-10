#include "page_manager.h"

#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_log.h>

#define TAG "PageManager"

void ChatPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display != nullptr) {
        display->ShowChatUi();
    }
}

void ChatPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    (void)display;
}

void PageManager::Initialize(CardputerAdvCarLcdDisplay* display, EmqxCarMqtt* mqtt) {
    display_ = display;
    mqtt_ = mqtt;
    car_page_.SetMqtt(mqtt);
    spider_page_.SetMqtt(mqtt);
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
        default:
            return nullptr;
    }
}

void PageManager::ShowPage(PageId id) {
    if (display_ == nullptr || id == current_) {
        return;
    }

    Page* next = GetPage(id);
    Page* current = GetPage(current_);
    if (next == nullptr || current == nullptr) {
        return;
    }

    if (id == PageId::Chat && mqtt_ != nullptr && mqtt_->run() != 0) {
        mqtt_->PublishCarCmd(0, mqtt_->speed());
    }

    ESP_LOGI(TAG, "Switch page %d -> %d", static_cast<int>(current_), static_cast<int>(id));
    current->OnLeave(display_);
    current_ = id;
    next->OnEnter(display_);
}

void PageManager::RefreshCurrentPage() {
    if (display_ == nullptr) {
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

void PageManager::Tick() {
    if (current_ == PageId::Car) {
        car_page_.Tick(display_);
    } else if (current_ == PageId::Spider) {
        spider_page_.Tick(display_);
    }
}

bool PageManager::HandleVehicleKey(const KeyEvent& event) {
    if (current_ == PageId::Car) {
        return car_page_.HandleKey(event);
    }
    if (current_ == PageId::Spider) {
        return spider_page_.HandleKey(event);
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
