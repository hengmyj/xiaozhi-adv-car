#pragma once

#include "page.h"
#include "page_id.h"

#include <lvgl.h>

#include <array>

class CardputerAdvCarLcdDisplay;

class ChatPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
};

class CarPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;

private:
    lv_obj_t* panel_ = nullptr;
};

class PageManager {
public:
    void Initialize(CardputerAdvCarLcdDisplay* display);
    void ShowPage(PageId id);
    void RefreshCurrentPage();
    PageId current_page() const { return current_; }

private:
    CardputerAdvCarLcdDisplay* display_ = nullptr;
    PageId current_ = PageId::Chat;
    ChatPage chat_page_;
    CarPage car_page_;
    std::array<Page*, 2> pages_{};

    Page* GetPage(PageId id);
};
