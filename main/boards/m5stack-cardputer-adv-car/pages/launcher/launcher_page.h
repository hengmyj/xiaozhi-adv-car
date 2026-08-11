#pragma once

#include "page.h"
#include "tca8418_keyboard.h"

#include <functional>
#include <lvgl.h>

class CardputerAdvCarLcdDisplay;

class LauncherPage : public Page {
public:
    using NavigateCallback = std::function<void(PageId id)>;

    void SetNavigateCallback(NavigateCallback cb) { navigate_ = std::move(cb); }

    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void BuildMyjLogo(lv_obj_t* parent);
    void RefreshTime(CardputerAdvCarLcdDisplay* display);
    lv_obj_t* MakeAppButton(lv_obj_t* parent, const char* badge, const char* title,
                            uint32_t color, int x, int y, int w, int h);

    NavigateCallback navigate_;
    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    int last_sec_ = -1;
    static constexpr int kAppCount = 7;
    lv_obj_t* btns_[kAppCount] = {};
};
