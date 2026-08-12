#pragma once

#include <cstdint>

// Cardputer ADV onboard IR emitter (same GPIO as Sparks).
#ifndef CARDPUTER_IR_TX_GPIO
#define CARDPUTER_IR_TX_GPIO 44
#endif

// ---------------------------------------------------------------------------
// Target: 三菱电机 ZFJ 系列 MSZ-ZFJ12VA (KFR-36GW/BpU) aka ZFJ12
// ---------------------------------------------------------------------------
enum class MjAcIrProtocol : uint8_t {
    MitsubishiAc = 0,    // MITSUBISHI_AC / IRMitsubishiAC (default for ZFJ12)
    Mitsubishi112 = 1,   // MITSUBISHI112 / IRMitsubishi112
    Mitsubishi136 = 2,   // MITSUBISHI136 / IRMitsubishi136
};

#ifndef MJ_AC_IR_PROTOCOL
#define MJ_AC_IR_PROTOCOL MjAcIrProtocol::MitsubishiAc
#endif

enum class MjAcMode : uint8_t {
    Cool = 0,
    Heat = 1,
    Dry = 2,
    Fan = 3,
    Auto = 4,
};

enum class MjAcFan : uint8_t {
    Auto = 0,
    Min = 1,
    Low = 2,
    Med = 3,
    High = 4,
    Max = 5,
};

struct MjAcState {
    bool power = true;
    MjAcMode mode = MjAcMode::Cool;
    uint8_t temp_c = 26;
    MjAcFan fan = MjAcFan::Auto;
};

class MitsubishiIrSender {
public:
    bool Initialize();
    bool Send(const MjAcState& state);
    // Drain queue + abort in-flight TX. Must be fast — called from page OnLeave.
    void Cancel();
    bool IsReady() const { return ready_; }
    bool IsBusy() const;
    static const char* ProtocolName();

private:
    bool ready_ = false;
    void* ac_ = nullptr;
};
