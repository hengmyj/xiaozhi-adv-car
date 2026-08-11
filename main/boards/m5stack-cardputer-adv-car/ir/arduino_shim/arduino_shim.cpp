#include "Arduino.h"

#include <esp_rom_sys.h>

void pinMode(uint8_t pin, uint8_t mode) {
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    // Reset any prior USB/JTAG/UART mux so GPIO 44 (Cardputer IR) is usable.
    gpio_reset_pin(gpio);
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_direction(gpio, (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

void digitalWrite(uint8_t pin, uint8_t val) {
    // Direct write is faster/more reliable for 38 kHz software IR PWM.
    gpio_set_level(static_cast<gpio_num_t>(pin), val ? 1 : 0);
}

void delayMicroseconds(uint32_t us) {
    // Busy-wait: required for accurate IR carrier bit-banging.
    esp_rom_delay_us(us);
}

void delay(uint32_t ms) {
    // Prefer busy-wait for short IR gaps so FreeRTOS preemption does not
    // stretch protocol timing. Longer sleeps still yield to the scheduler.
    if (ms == 0) {
        return;
    }
    if (ms <= 50) {
        esp_rom_delay_us(ms * 1000UL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t micros() {
    return static_cast<uint32_t>(esp_timer_get_time());
}

uint32_t millis() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}
