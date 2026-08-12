#pragma once

#include "car_page.h"
#include "clock_page.h"
#include "cursor_page.h"
#include "emqx_mqtt_client.h"
#include "launcher_page.h"
#include "matrix_page.h"
#include "mj_ac_page.h"
#include "page.h"
#include "page_id.h"
#include "radio_page.h"
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
    bool IsAcPage() const;
    bool IsLauncherPage() const;
    bool IsExclusivePage() const;
    PageId current_page() const { return current_; }

private:
    CardputerAdvCarLcdDisplay* display_ = nullptr;
    EmqxCarMqtt* mqtt_ = nullptr;
    PageId current_ = PageId::Chat;
    bool switching_ = false;
    ChatPage chat_page_;
    CarPage car_page_;
    SpiderPage spider_page_;
    MjAcPage mj_ac_page_;
    LauncherPage launcher_page_;
    ClockPage clock_page_;
    MatrixPage matrix_page_;
    CursorPage cursor_page_;  // Music (mic visualizer)
    RadioPage radio_page_;

    Page* GetPage(PageId id);
    void RecoverToChat(const char* reason);
    void ReleaseOtherExclusiveUi(PageId keep);
    // After Fn+1 Chat is fully shown (switching_ already false). Never from
    // Radio/Music OnLeave — Opus rebuild during exclusive teardown fragments
    // no-PSRAM heap. Boot Initialize must not call this (codec_ still null).
    void ScheduleChatAudioRestore(int retry = 0);
};
