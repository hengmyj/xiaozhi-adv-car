#pragma once

#include "page_id.h"

#include <lvgl.h>

class CardputerAdvCarLcdDisplay;

class Page {
public:
    virtual ~Page() = default;
    virtual void OnEnter(CardputerAdvCarLcdDisplay* display) = 0;
    virtual void OnLeave(CardputerAdvCarLcdDisplay* display) = 0;
    /** Exclusive pages: root panel; Chat returns nullptr (uses chat UI). */
    virtual lv_obj_t* GetRootPanel() const { return nullptr; }
};
