#pragma once

// EMQX public broker — car control channel (separate from xiaozhi cloud MQTT)
#define EMQX_BROKER_HOST "broker-cn.emqx.io"
#define EMQX_BROKER_PORT 1883

#define EMQX_TOPIC_CAR_CMD "car/cmd"
#define EMQX_TOPIC_FOC_CMD "foc/cmd"
#define EMQX_TOPIC_CAR_STATE "car/state"

#define EMQX_RECONNECT_MS 5000
#define EMQX_DEFAULT_SPEED 30
#define EMQX_PUBLISH_QOS 1
