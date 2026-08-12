#pragma once

#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

class CardputerAdvCarLcdDisplay;

enum class RadioPlayState : uint8_t {
    Idle = 0,
    Connecting,
    Playing,
    Paused,
    Error,
};

class RadioPage : public Page {
public:
    void OnEnter(CardputerAdvCarLcdDisplay* display) override;
    void OnLeave(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

    void NotifyLevel(float level);
    void NotifyPlaying();
    void SetStatusHint(const char* hint);
    bool IsStreamRunning() const;
    bool IsUserPaused() const;
    int StationIndex() const;
    uint32_t StationGeneration() const;

private:
    void BuildPanel(CardputerAdvCarLcdDisplay* display);
    void DestroyPanel(CardputerAdvCarLcdDisplay* display);
    void UpdateUi(CardputerAdvCarLcdDisplay* display);
    void StartStream();
    void StopStream();
    void CaptureAudioExclusive();
    void ReleaseAudioExclusive();
    void SelectStation(int index);
    void TogglePause();
    void AdjustVolume(int delta);

    static void StreamTask(void* arg);

    static constexpr int kBarCount = 16;
    static constexpr int kStationCount = 2;

    CardputerAdvCarLcdDisplay* display_ = nullptr;
    bool active_ = false;
    bool audio_exclusive_ = false;

    lv_obj_t* panel_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* station_label_ = nullptr;
    lv_obj_t* listening_label_ = nullptr;
    lv_obj_t* play_label_ = nullptr;
    lv_obj_t* vol_label_ = nullptr;
    lv_obj_t* bars_[kBarCount] = {};

    std::atomic<RadioPlayState> play_state_{RadioPlayState::Idle};
    std::atomic<bool> stream_run_{false};
    std::atomic<bool> user_paused_{false};
    std::atomic<int> station_index_{1};  // default Music / FM974
    std::atomic<uint32_t> station_gen_{0};
    std::atomic<float> level_{0.0f};
    char status_hint_[40] = {};
    TaskHandle_t stream_task_ = nullptr;
};
