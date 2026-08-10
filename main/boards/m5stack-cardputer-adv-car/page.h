#pragma once

#include "page_id.h"

class CardputerAdvCarLcdDisplay;

class Page {
public:
    virtual ~Page() = default;
    virtual void OnEnter(CardputerAdvCarLcdDisplay* display) = 0;
    virtual void OnLeave(CardputerAdvCarLcdDisplay* display) = 0;
};
