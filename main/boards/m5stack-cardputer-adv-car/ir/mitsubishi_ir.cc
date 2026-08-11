#include "mitsubishi_ir.h"

#include "Arduino.h"
#include "ir_Mitsubishi.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define TAG "MitsubishiIr"

namespace {

constexpr int kIrQueueDepth = 4;
// Extra full-frame retransmission (many wall remotes fire 2ù3 bursts).
// Library already applies kMitsubishiACMinRepeat (=1 ? 2 frames); we send
// the whole burst a second time for better range / reliability.
constexpr int kExtraBurstCount = 1;
constexpr int kBurstGapMs = 40;

struct IrSendRequest {
    MjAcState state;
};

IRMitsubishiAC* AsAc(void* ptr) {
    return static_cast<IRMitsubishiAC*>(ptr);
}

uint8_t MapMode(MjAcMode mode) {
    switch (mode) {
        case MjAcMode::Heat:
            return kMitsubishiAcHeat;
        case MjAcMode::Dry:
            return kMitsubishiAcDry;
        case MjAcMode::Fan:
            return kMitsubishiAcFan;
        case MjAcMode::Auto:
            return kMitsubishiAcAuto;
        case MjAcMode::Cool:
        default:
            return kMitsubishiAcCool;
    }
}

uint8_t MapFan(MjAcFan fan) {
    switch (fan) {
        case MjAcFan::Min:
            return 1;
        case MjAcFan::Low:
            return 2;
        case MjAcFan::Med:
            return 3;
        case MjAcFan::High:
            return 4;
        case MjAcFan::Max:
            return kMitsubishiAcFanMax;
        case MjAcFan::Auto:
        default:
            return kMitsubishiAcFanAuto;
    }
}

void LogRawState(IRMitsubishiAC* ac) {
    const uint8_t* raw = ac->getRaw();
    ESP_LOGI(TAG,
             "TX GPIO=%d protocol=%s raw=%02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             CARDPUTER_IR_TX_GPIO, MitsubishiIrSender::ProtocolName(),
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15],
             raw[16], raw[17]);
}

void ApplyState(IRMitsubishiAC* ac, const MjAcState& state) {
    ac->setPower(state.power);
    ac->setTemp(state.temp_c);
    ac->setMode(MapMode(state.mode));
    ac->setFan(MapFan(state.fan));
}

void TransmitNow(IRMitsubishiAC* ac, const MjAcState& state) {
    ApplyState(ac, state);
    LogRawState(ac);

    // Raise priority so software 38 kHz PWM is less often preempted.
    UBaseType_t prev = uxTaskPriorityGet(nullptr);
    vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 1);
    const int64_t t0 = esp_timer_get_time();

    // First burst (includes library min-repeat). Then one extra full burst.
    ac->send();
    for (int i = 0; i < kExtraBurstCount; ++i) {
        delay(kBurstGapMs);
        ac->send();
    }

    const int64_t dt = esp_timer_get_time() - t0;
    vTaskPrioritySet(nullptr, prev);

    ESP_LOGI(TAG,
             "sendAc done protocol=%s bursts=%d in %lld us "
             "(IR LED is infrared ù not visible to eye)",
             MitsubishiIrSender::ProtocolName(), 1 + kExtraBurstCount,
             static_cast<long long>(dt));
}

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
IRMitsubishiAC* g_ac = nullptr;

void IrWorkerTask(void* /*arg*/) {
    IrSendRequest req{};
    while (true) {
        if (xQueueReceive(g_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (g_ac == nullptr) {
            ESP_LOGW(TAG, "IR worker: AC object null");
            continue;
        }
        // Coalesce: if more requests queued, keep only the latest state.
        IrSendRequest newest = req;
        while (xQueueReceive(g_queue, &req, 0) == pdTRUE) {
            newest = req;
        }
        ESP_LOGI(TAG, "IR worker TX protocol=%s power=%d temp=%u mode=%d fan=%d",
                 MitsubishiIrSender::ProtocolName(),
                 newest.state.power, newest.state.temp_c,
                 static_cast<int>(newest.state.mode),
                 static_cast<int>(newest.state.fan));
        TransmitNow(g_ac, newest.state);
    }
}

}  // namespace

const char* MitsubishiIrSender::ProtocolName() {
    switch (MJ_AC_IR_PROTOCOL) {
        case MjAcIrProtocol::Mitsubishi112:
            return "MITSUBISHI112";
        case MjAcIrProtocol::Mitsubishi136:
            return "MITSUBISHI136";
        case MjAcIrProtocol::MitsubishiAc:
        default:
            return "MITSUBISHI_AC";
    }
}

bool MitsubishiIrSender::Initialize() {
    static_assert(static_cast<int>(MJ_AC_IR_PROTOCOL) ==
                      static_cast<int>(MjAcIrProtocol::MitsubishiAc),
                  "Only MITSUBISHI_AC is compiled in; change MJ_AC_IR_PROTOCOL "
                  "and enable the matching SEND_* flag in CMakeLists.");

    if (ac_ == nullptr) {
        // Cardputer ADV IR emitter on GPIO 44 (active-high via onboard driver).
        ac_ = new IRMitsubishiAC(CARDPUTER_IR_TX_GPIO, /*inverted=*/false,
                                 /*modulation=*/true);
    }

    auto* ac = AsAc(ac_);
    ac->begin();
    g_ac = ac;

    if (g_queue == nullptr) {
        g_queue = xQueueCreate(kIrQueueDepth, sizeof(IrSendRequest));
    }
    if (g_task == nullptr && g_queue != nullptr) {
        // Dedicated task so keyboard ISR/task is never blocked by ~100 ms AC frames.
        xTaskCreatePinnedToCore(IrWorkerTask, "mj_ir_tx", 4096, nullptr, 5, &g_task, 1);
    }

    ready_ = (g_queue != nullptr && g_task != nullptr && ac_ != nullptr);
    ESP_LOGI(TAG, "IR TX ready on GPIO %d protocol=%s ready=%d",
             CARDPUTER_IR_TX_GPIO, ProtocolName(), static_cast<int>(ready_));
    return ready_;
}

bool MitsubishiIrSender::Send(const MjAcState& state) {
    if (!ready_ && !Initialize()) {
        return false;
    }
    if (g_queue == nullptr) {
        return false;
    }

    IrSendRequest req{};
    req.state = state;

    // Non-blocking: drop oldest if full so UI stays responsive.
    if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
        IrSendRequest discarded{};
        xQueueReceive(g_queue, &discarded, 0);
        if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
            ESP_LOGW(TAG, "IR queue full, send dropped");
            return false;
        }
    }
    ESP_LOGI(TAG, "IR queued protocol=%s power=%d temp=%u mode=%d fan=%d",
             ProtocolName(), state.power, state.temp_c,
             static_cast<int>(state.mode), static_cast<int>(state.fan));
    return true;
}
