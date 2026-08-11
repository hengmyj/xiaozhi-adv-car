#pragma once

#include <stdint.h>
#include <string>
#include <cmath>

#ifndef double_t
typedef double double_t;
#endif

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif
#ifndef INPUT
#define INPUT 0
#endif

#define F(x) (x)

class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}

    String substring(unsigned int begin, unsigned int end = 0) const {
        if (end == 0 || end > size()) {
            end = static_cast<unsigned int>(size());
        }
        if (begin >= size()) {
            return String();
        }
        return String(substr(begin, end - begin));
    }
};

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
void delayMicroseconds(uint32_t us);
void delay(uint32_t ms);
uint32_t micros();
uint32_t millis();
