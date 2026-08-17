#include "page_manager.h"

#include "application.h"
#include "board.h"
#include "cardputer_adv_lcd_display.h"
#include "display/display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "PageManager"

namespace {

size_t FreeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t LargestHeap() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

bool PanelVisible(lv_obj_t* panel) {
    return panel != nullptr && !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

bool IsDashboardPage(PageId id) {
    return id == PageId::Car || id == PageId::Spider || id == PageId::MjAc;
}

// Keep Car/Spider/IceBox panels when switching among them (IceBox→Car used to
// freeze LVGL). Clock/Matrix/dashboards going to Launcher/Chat/Radio must drop
// the hidden panel so MP3 can get a contiguous internal-SRAM block.
bool ShouldReleaseResidentUi(PageId from, PageId to) {
    if (IsDashboardPage(from) && IsDashboardPage(to)) {
        return false;
    }
    // Drop the hidden Launcher (46 objs, ~10KB) when entering the memory-hungry
    // games — no-PSRAM heap was ~11KB with it resident, too low for image draws.
    if (from == PageId::Launcher && (to == PageId::Snake || to == PageId::Dino)) {
        return true;
    }
    return from == PageId::Car || from == PageId::Spider || from == PageId::MjAc ||
           from == PageId::Clock || from == PageId::Matrix || from == PageId::Snake ||
           from == PageId::Dino;
}

}  // namespace

void ChatPage::OnEnter(CardputerAdvCarLcdDisplay* display) {
    if (display != nullptr) {
        display->ShowChatUi();
    }
    // UI only. Do not RestoreAudioModels here: ShowPage still has switching_
    // set, and Radio/Music may still be tearing down. Opus/mic rebuild is
    // ScheduleChatAudioRestore after ShowPage clears switching_.
}

void ChatPage::OnLeave(CardputerAdvCarLcdDisplay* display) {
    (void)display;
}

void PageManager::Initialize(CardputerAdvCarLcdDisplay* display, EmqxCarMqtt* mqtt) {
    display_ = display;
    mqtt_ = mqtt;
    car_page_.SetMqtt(mqtt);
    spider_page_.SetMqtt(mqtt);
    launcher_page_.SetNavigateCallback([this](PageId id) {
        // Defer off keyboard task (same as Fn page switch).
        Application::GetInstance().Schedule([this, id]() { ShowPage(id); });
    });
    current_ = PageId::Chat;
    // Show Chat UI only. Do not ChatPage::OnEnter / ScheduleChatAudioRestore:
    // Application::Initialize calls SetupUI before AudioService::Initialize,
    // so codec_ is still null (9a6221b RestoreAudioModels → LoadProhibited).
    if (display_ != nullptr) {
        display_->ShowChatUi();
    }
}

Page* PageManager::GetPage(PageId id) {
    switch (id) {
        case PageId::Chat:
            return &chat_page_;
        case PageId::Car:
            return &car_page_;
        case PageId::Spider:
            return &spider_page_;
        case PageId::MjAc:
            return &mj_ac_page_;
        case PageId::Launcher:
            return &launcher_page_;
        case PageId::Clock:
            return &clock_page_;
        case PageId::Matrix:
            return &matrix_page_;
        case PageId::Music:
            return &cursor_page_;
        case PageId::Radio:
            return &radio_page_;
        case PageId::Snake:
            return &snake_page_;
        case PageId::Dino:
            return &dino_page_;
        default:
            return nullptr;
    }
}

void PageManager::ReleaseOtherExclusiveUi(PageId keep) {
    static const PageId kExclusive[] = {
        PageId::Car,      PageId::Spider, PageId::MjAc,  PageId::Launcher,
        PageId::Clock,    PageId::Matrix, PageId::Music, PageId::Radio,
        PageId::Snake,    PageId::Dino,
    };
    const size_t before = FreeHeap();
    const size_t before_largest = LargestHeap();
    int n = 0;
    for (PageId id : kExclusive) {
        if (id == keep) {
            continue;
        }
        Page* page = GetPage(id);
        if (page == nullptr || page->GetRootPanel() == nullptr) {
            continue;
        }
        ESP_LOGI(TAG, "sweep destroy page %d heap=%u largest=%u", static_cast<int>(id),
                 static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()));
        page->ReleaseResidentUi(display_);
        ++n;
    }
    ESP_LOGI(TAG, "sweep destroyed %d exclusive panels keep=%d heap %u->%u largest %u->%u", n,
             static_cast<int>(keep), static_cast<unsigned>(before),
             static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(before_largest),
             static_cast<unsigned>(LargestHeap()));
}

void PageManager::RecoverToChat(const char* reason) {
    ESP_LOGE(TAG, "RecoverToChat: %s (was page %d) heap=%u largest=%u", reason,
             static_cast<int>(current_), static_cast<unsigned>(FreeHeap()),
             static_cast<unsigned>(LargestHeap()));
    Page* stuck = GetPage(current_);
    if (stuck != nullptr && current_ != PageId::Chat) {
        // OnLeave first (Music drops mic; Radio stops stream; hide failed panel).
        // Never RestoreAudioModels here — wait until Chat UI is shown.
        stuck->OnLeave(display_);
        stuck->ReleaseResidentUi(display_);
    }
    current_ = PageId::Chat;
    chat_page_.OnEnter(display_);
    if (display_ != nullptr && !display_->IsChatUiVisible()) {
        ESP_LOGE(TAG, "Chat UI still hidden after recover; trying Launcher heap=%u",
                 static_cast<unsigned>(FreeHeap()));
        chat_page_.OnLeave(display_);
        current_ = PageId::Launcher;
        launcher_page_.OnEnter(display_);
        if (!PanelVisible(launcher_page_.GetRootPanel())) {
            ESP_LOGE(TAG, "Launcher also failed; force ShowChatUi");
            current_ = PageId::Chat;
            if (display_ != nullptr) {
                display_->ShowChatUi();
            }
        }
    }
    if (current_ == PageId::Chat) {
        ScheduleChatAudioRestore();
    }
}

void PageManager::ScheduleChatAudioRestore(int retry) {
    // Next main-loop tick: ShowPage has returned, switching_ is already false,
    // Chat UI is on screen. Never from Radio/Music OnLeave. No-op until
    // ReleaseAudioModels — boot Initialize / first Chat must not RecycleDevice.
    Application::GetInstance().Schedule([this, retry]() {
        if (switching_) {
            if (retry < 2) {
                ESP_LOGW(TAG, "Chat audio restore deferred switching retry=%d", retry);
                ScheduleChatAudioRestore(retry + 1);
            } else {
                ESP_LOGE(TAG, "Chat audio restore dropped: still switching page=%d",
                         static_cast<int>(current_));
            }
            return;
        }
        if (current_ != PageId::Chat) {
            ESP_LOGW(TAG, "Chat audio restore dropped page=%d", static_cast<int>(current_));
            return;
        }
        if (display_ == nullptr || !display_->IsChatUiVisible()) {
            ESP_LOGW(TAG, "Chat audio restore: Chat UI not visible, restoring audio anyway");
            if (display_ != nullptr) {
                display_->ShowChatUi();
            }
        }
        auto& app = Application::GetInstance();
        auto& audio = app.GetAudioService();
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            ESP_LOGW(TAG, "Chat audio restore skipped: codec null");
            return;
        }
        if (!audio.AudioModelsReleased()) {
            ESP_LOGI(TAG,
                     "Chat audio restore skipped: never released enc=%d dec=%d (boot-safe)",
                     audio.HasOpusEncoder() ? 1 : 0, audio.HasOpusDecoder() ? 1 : 0);
            return;
        }
        ESP_LOGI(TAG,
                 "Chat audio restore run state=%d released=%d enc=%d dec=%d ext=%d in=%d out=%d",
                 static_cast<int>(app.GetDeviceState()), audio.AudioModelsReleased() ? 1 : 0,
                 audio.HasOpusEncoder() ? 1 : 0, audio.HasOpusDecoder() ? 1 : 0,
                 audio.IsExternalPlaybackActive() ? 1 : 0, codec->input_enabled() ? 1 : 0,
                 codec->output_enabled() ? 1 : 0);
        app.RestoreAudioRouting();
        if (!codec->input_enabled()) {
            codec->EnableInput(true);
        }
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        const auto& stats = audio.GetDebugStatistics();
        ESP_LOGI(TAG,
                 "Chat audio restore done heap=%u largest=%u in=%d out=%d enc=%d dec=%d ww=%d vp=%d "
                 "in_cnt=%u enc_cnt=%u",
                 static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()),
                 codec->input_enabled() ? 1 : 0, codec->output_enabled() ? 1 : 0,
                 audio.HasOpusEncoder() ? 1 : 0, audio.HasOpusDecoder() ? 1 : 0,
                 audio.IsWakeWordRunning() ? 1 : 0, audio.IsAudioProcessorRunning() ? 1 : 0,
                 stats.input_count, stats.encode_count);
    });
}

void PageManager::ShowPage(PageId id) {
    if (display_ == nullptr || id == current_) {
        return;
    }
    if (switching_) {
        ESP_LOGW(TAG, "ShowPage re-entry ignored %d -> %d", static_cast<int>(current_),
                 static_cast<int>(id));
        return;
    }

    Page* next = GetPage(id);
    Page* current = GetPage(current_);
    if (next == nullptr || current == nullptr) {
        return;
    }

    switching_ = true;
    const PageId prev = current_;
    ESP_LOGI(TAG, "ShowPage enter %d -> %d heap=%u largest=%u", static_cast<int>(prev),
             static_cast<int>(id), static_cast<unsigned>(FreeHeap()),
             static_cast<unsigned>(LargestHeap()));

    if (id == PageId::Chat && mqtt_ != nullptr && mqtt_->run() != 0) {
        mqtt_->PublishCarCmd(0, mqtt_->speed());
    }

    // Leave-first for exclusive→exclusive (IceBox IR / Radio / …). Chat→page
    // still enters first so HideChatUi never leaves a WiFi-only blank frame.
    if (prev != PageId::Chat) {
        current->OnLeave(display_);
        if (ShouldReleaseResidentUi(prev, id)) {
            ESP_LOGI(TAG, "release resident UI %d before %d heap=%u largest=%u",
                     static_cast<int>(prev), static_cast<int>(id),
                     static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()));
            current->ReleaseResidentUi(display_);
            ESP_LOGI(TAG, "released resident UI %d heap=%u largest=%u", static_cast<int>(prev),
                     static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()));
        }
        if (id == PageId::Radio) {
            ReleaseOtherExclusiveUi(PageId::Radio);
        }
        current_ = id;
        next->OnEnter(display_);
    } else {
        if (id == PageId::Radio) {
            ReleaseOtherExclusiveUi(PageId::Radio);
        }
        current_ = id;
        next->OnEnter(display_);
        current->OnLeave(display_);
    }

    if (id == PageId::Chat) {
        if (!display_->IsChatUiVisible()) {
            RecoverToChat("chat UI still hidden after switch to Chat");
            switching_ = false;
            ESP_LOGI(TAG, "ShowPage leave (recovered) heap=%u largest=%u",
                     static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()));
            return;
        }
    } else {
        lv_obj_t* panel = next->GetRootPanel();
        if (!PanelVisible(panel)) {
            RecoverToChat("next page panel null/hidden after OnEnter");
            switching_ = false;
            ESP_LOGI(TAG, "ShowPage leave (recovered) heap=%u largest=%u",
                     static_cast<unsigned>(FreeHeap()), static_cast<unsigned>(LargestHeap()));
            return;
        }
        // Keep exclusive panel above any chat chrome that UpdateStatusBar may touch.
        DisplayLockGuard lock(display_);
        lv_obj_move_foreground(panel);
    }

    ESP_LOGI(TAG, "ShowPage leave %d -> %d ok heap=%u largest=%u", static_cast<int>(prev),
             static_cast<int>(current_), static_cast<unsigned>(FreeHeap()),
             static_cast<unsigned>(LargestHeap()));
    switching_ = false;
    if (current_ == PageId::Chat) {
        ScheduleChatAudioRestore();
    }
}

void PageManager::RefreshCurrentPage() {
    if (display_ == nullptr || switching_) {
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

bool PageManager::IsAcPage() const {
    return current_ == PageId::MjAc;
}

bool PageManager::IsLauncherPage() const {
    return current_ == PageId::Launcher;
}

bool PageManager::IsExclusivePage() const {
    return current_ != PageId::Chat;
}

void PageManager::Tick() {
    if (switching_) {
        return;
    }
    // Soft watchdog: exclusive page must keep a visible panel.
    if (current_ != PageId::Chat) {
        Page* page = GetPage(current_);
        if (page != nullptr && !PanelVisible(page->GetRootPanel())) {
            RecoverToChat("tick watchdog: panel missing");
            return;
        }
    }

    if (current_ == PageId::Car) {
        car_page_.Tick(display_);
    } else if (current_ == PageId::Spider) {
        spider_page_.Tick(display_);
    } else if (current_ == PageId::MjAc) {
        mj_ac_page_.Tick(display_);
    } else if (current_ == PageId::Clock) {
        clock_page_.Tick(display_);
    } else if (current_ == PageId::Matrix) {
        matrix_page_.Tick(display_);
    } else if (current_ == PageId::Music) {
        cursor_page_.Tick(display_);
    } else if (current_ == PageId::Radio) {
        radio_page_.Tick(display_);
    } else if (current_ == PageId::Launcher) {
        launcher_page_.Tick(display_);
    } else if (current_ == PageId::Snake) {
        snake_page_.Tick(display_);
    } else if (current_ == PageId::Dino) {
        dino_page_.Tick(display_);
    }
}

bool PageManager::HandleVehicleKey(const KeyEvent& event) {
    if (current_ == PageId::Car) {
        return car_page_.HandleKey(event);
    }
    if (current_ == PageId::Spider) {
        return spider_page_.HandleKey(event);
    }
    if (current_ == PageId::MjAc) {
        return mj_ac_page_.HandleKey(event);
    }
    if (current_ == PageId::Launcher) {
        return launcher_page_.HandleKey(event);
    }
    // Clock / Matrix / Music / Radio: page-owned keys (Fn handled upstream).
    if (current_ == PageId::Clock || current_ == PageId::Matrix) {
        return true;
    }
    if (current_ == PageId::Music) {
        return true;
    }
    if (current_ == PageId::Radio) {
        return radio_page_.HandleKey(event);
    }
    if (current_ == PageId::Snake) {
        return snake_page_.HandleKey(event);
    }
    if (current_ == PageId::Dino) {
        return dino_page_.HandleKey(event);
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
