#pragma once

#include <cstdint>

enum class PageId : uint8_t {
    Chat = 1,
    Car = 2,
    Spider = 3,
    MjAc = 4,
    Launcher = 5,
    Clock = 6,
    Matrix = 7,
    Music = 8,  // mic visualizer (was Cursor; real Cursor later)
    Radio = 9,  // CNR 央广 live stream
};
