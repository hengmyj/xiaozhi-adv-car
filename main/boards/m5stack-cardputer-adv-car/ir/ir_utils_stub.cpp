#include <cstdint>

uint8_t sumBytes(const uint8_t* const start, const uint16_t length, const uint8_t init) {
    uint8_t sum = init;
    for (uint16_t i = 0; i < length; ++i) {
        sum += start[i];
    }
    return sum;
}
