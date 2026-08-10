#pragma once

#include "car_page.h"
#include "emqx_mqtt_client.h"
#include "page.h"
#include "page_id.h"
#include "spider_page.h"
#include "tca8418_keyboard.h"

class CardputerAdvCarLcdDisplay;

class ChatPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
};

class PageManager {
public:
    void Initialize(CardputerAdvCarLcdDisplay* display, EmqxCarMqtt* mqtt);
    void ShowPage(PageId id);
    void RefreshCurrentPage();
    void Tick();
    bool HandleVehicleKey(const KeyEvent& event);
    bool HandleVehicleLegacyKey(LegacyKeyCode key);
    bool IsVehiclePage() const;
    PageId current_page() const { return current_; }

private:
    CardputerAdvCarLcdDisplay* display_ = nullptr;
    EmqxCarMqtt* mqtt_ = nullptr;
    PageId current_ = PageId::Chat;
    ChatPage chat_page_;
    CarPage car_page_;
    SpiderPage spider_page_;

    Page* GetPage(PageId id);
};
