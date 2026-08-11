#pragma once

#include <cJSON.h>
#include <cstdint>
#include <esp_timer.h>

struct CarState {
    int run = 0;
    int speed = 0;
    int pwm = 0;
    int64_t updated_at_us = 0;
    bool valid = false;
};

inline bool ParseCarStateJson(const char* json, size_t len, CarState& out) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) {
        return false;
    }
    cJSON* run = cJSON_GetObjectItem(root, "run");
    cJSON* speed = cJSON_GetObjectItem(root, "speed");
    cJSON* pwm = cJSON_GetObjectItem(root, "pwm");
    out.run = cJSON_IsNumber(run) ? run->valueint : 0;
    out.speed = cJSON_IsNumber(speed) ? speed->valueint : 0;
    out.pwm = cJSON_IsNumber(pwm) ? pwm->valueint : 0;
    out.updated_at_us = esp_timer_get_time();
    out.valid = true;
    cJSON_Delete(root);
    return true;
}
