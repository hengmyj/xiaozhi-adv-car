#include "emqx_mqtt_client.h"

#include "system_info.h"

#include <esp_log.h>
#include <wifi_manager.h>

#include <cstdio>

#define TAG "EmqxCarMqtt"

void EmqxCarMqtt::Initialize(NetworkInterface* network) {
    network_ = network;
    if (network_ == nullptr) {
        return;
    }

    std::string mac = SystemInfo::GetMacAddress();
    uint8_t b4 = 0;
    uint8_t b5 = 0;
    if (mac.size() >= 17) {
        unsigned v4 = 0;
        unsigned v5 = 0;
        if (sscanf(mac.c_str() + 12, "%02x:%02x", &v4, &v5) == 2) {
            b4 = static_cast<uint8_t>(v4);
            b5 = static_cast<uint8_t>(v5);
        }
    }
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "xiaozhi-adv-car-%02x%02x", b4, b5);
    client_id_ = id_buf;

    mqtt_ = network_->CreateMqtt(1);
    if (mqtt_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return;
    }

    mqtt_->SetKeepAlive(30);
    mqtt_->OnConnected([this]() {
        connected_ = true;
        ESP_LOGI(TAG, "Connected as %s", client_id_.c_str());
        mqtt_->Subscribe(EMQX_TOPIC_CAR_STATE, EMQX_PUBLISH_QOS);
    });
    mqtt_->OnDisconnected([this]() {
        connected_ = false;
        ESP_LOGW(TAG, "Disconnected");
    });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        OnMessage(topic, payload);
    });
}

void EmqxCarMqtt::Tick() {
    if (mqtt_ == nullptr) {
        return;
    }

    if (!WifiManager::GetInstance().IsConnected()) {
        if (connected_) {
            mqtt_->Disconnect();
            connected_ = false;
        }
        return;
    }

    if (mqtt_->IsConnected()) {
        connected_ = true;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - last_reconnect_us_ < static_cast<int64_t>(EMQX_RECONNECT_MS) * 1000) {
        return;
    }
    last_reconnect_us_ = now;
    TryConnect();
}

void EmqxCarMqtt::TryConnect() {
    if (mqtt_ == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "Connecting to %s:%d ...", EMQX_BROKER_HOST, EMQX_BROKER_PORT);
    if (mqtt_->Connect(EMQX_BROKER_HOST, EMQX_BROKER_PORT, client_id_, "", "")) {
        connected_ = true;
    } else {
        connected_ = false;
        ESP_LOGW(TAG, "Connect failed, err=%d", mqtt_->GetLastError());
    }
}

void EmqxCarMqtt::OnMessage(const std::string& topic, const std::string& payload) {
    if (topic != EMQX_TOPIC_CAR_STATE) {
        return;
    }
    CarState parsed;
    if (ParseCarStateJson(payload.c_str(), payload.size(), parsed)) {
        car_state_ = parsed;
        local_run_ = parsed.run;
        if (parsed.speed > 0) {
            speed_ = parsed.speed;
        }
    }
}

void EmqxCarMqtt::SetSpeed(int speed) {
    if (speed < 0) {
        speed = 0;
    } else if (speed > 100) {
        speed = 100;
    }
    speed_ = speed;
}

bool EmqxCarMqtt::PublishCarCmd(int run, int speed) {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Publish car/cmd skipped — MQTT offline");
        return false;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"run\":%d,\"speed\":%d}", run, speed);
    local_run_ = run;
    speed_ = speed;
    ESP_LOGI(TAG, "car/cmd %s", buf);
    return mqtt_->Publish(EMQX_TOPIC_CAR_CMD, buf, EMQX_PUBLISH_QOS);
}

bool EmqxCarMqtt::PublishFocCmd(int dir, int speed) {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Publish foc/cmd skipped — MQTT offline");
        return false;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"dir\":%d,\"speed\":%d}", dir, speed);
    local_dir_ = dir;
    speed_ = speed;
    ESP_LOGI(TAG, "foc/cmd %s", buf);
    return mqtt_->Publish(EMQX_TOPIC_FOC_CMD, buf, EMQX_PUBLISH_QOS);
}
