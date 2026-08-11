#pragma once

#include "car_state.h"
#include "emqx_config.h"

#include <mqtt.h>
#include <network_interface.h>

#include <memory>
#include <string>

class EmqxCarMqtt {
public:
    void Initialize(NetworkInterface* network);
    void Tick();

    bool IsConnected() const { return connected_; }
    const CarState& car_state() const { return car_state_; }

    int speed() const { return speed_; }
    int run() const { return local_run_; }
    int dir() const { return local_dir_; }

    bool PublishCarCmd(int run, int speed);
    bool PublishFocCmd(int dir, int speed);

    void SetSpeed(int speed);

private:
    void TryConnect();
    void OnMessage(const std::string& topic, const std::string& payload);

    NetworkInterface* network_ = nullptr;
    std::unique_ptr<Mqtt> mqtt_;
    std::string client_id_;
    CarState car_state_;
    bool connected_ = false;
    int64_t last_reconnect_us_ = 0;
    int speed_ = EMQX_DEFAULT_SPEED;
    int local_run_ = 0;
    int local_dir_ = 0;
};
