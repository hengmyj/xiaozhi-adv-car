#pragma once

#include "page.h"
#include "tca8418_keyboard.h"

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
    void ReleaseResidentUi(CardputerAdvCarLcdDisplay* display) override;
    void Tick(CardputerAdvCarLcdDisplay* display);
    bool HandleKey(const KeyEvent& event);
    lv_obj_t* GetRootPanel() const override { return panel_; }

    // Called from the stream task.
    void NotifyLevel(float level);
    void NotifyBandLevels(const float* energies, int count);
    void NotifyPlaying();
    void SetStatusHint(const char* hint);
    bool IsStreamRunning() const { return stream_run_.load(); }
    bool IsUserPaused() const { return user_paused_.load(); }
    int StationIndex() const { return station_index_.load(); }
    uint32_t StationGeneration() const { return station_gen_.load(); }
    // Stream task publishes its HTTP client so OnLeave can close the socket and
    // unblock open/read without deleting the task.
    void PublishActiveHttp(void* client);
    void ClearActiveHttp(void* expect);

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
    void AbortActiveHttp();
    bool WaitStreamExit(TickType_t ticks, const char* why);

    static void StreamTask(void* arg);

    static constexpr int kBarCount = 24;

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
    float band_levels_[kBarCount] = {};
    float band_peak_ = 200.0f;

    std::atomic<RadioPlayState> play_state_{RadioPlayState::Idle};
    std::atomic<bool> stream_run_{false};
    std::atomic<bool> user_paused_{false};
    std::atomic<bool> stream_alive_{false};
    std::atomic<int> station_index_{1};
    std::atomic<uint32_t> station_gen_{0};
    // Bumped on every OnEnter/OnLeave so a deferred StartStream from a prior visit
    // cannot start audio after the user has already left (or left and come back).
    std::atomic<uint32_t> enter_gen_{0};
    std::atomic<float> level_{0.0f};
    // esp_http_client_handle_t published by the stream task so StopStream can
    // close the socket and unblock a stuck open/read without vTaskDelete.
    std::atomic<void*> active_http_{nullptr};
    char status_hint_[40] = {};

    // The stream task never touches its own handle; it only gives stream_done_ on
    // the way out. StopStream joins on that instead of vTaskDelete()-ing a task
    // that may already have destroyed itself.
    TaskHandle_t stream_task_ = nullptr;
    SemaphoreHandle_t stream_done_ = nullptr;
};
