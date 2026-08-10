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

void CarPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    if (panel_ == nullptr) {
        panel_ = lv_obj_create(display->GetScreen());
        lv_obj_set_size(panel_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        lv_obj_set_style_pad_all(panel_, 8, 0);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0x101010), 0);
        lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* title = lv_label_create(panel_);
        lv_label_set_text(title, "CAR");
        lv_obj_set_style_text_color(title, lv_color_hex(0x00FF88), 0);

        lv_obj_t* hint = lv_label_create(panel_);
        lv_label_set_text(hint, "Fn+1 Chat  Fn+2 Car");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    }

    display->HideChatUi();
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(panel_);
}

void CarPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    if (display == nullptr || panel_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(display);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

void PageManager::Initialize(CardputerAdvCarLcdDisplay* display) {
    display_ = display;
    pages_[0] = &chat_page_;
    pages_[1] = &car_page_;
    current_ = PageId::Chat;
    chat_page_.OnEnter(display_);
}

Page* PageManager::GetPage(PageId id) {
    switch (id) {
        case PageId::Chat:
            return &chat_page_;
        case PageId::Car:
            return &car_page_;
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
