#ifndef HMS_MQTT_CLIENT_H
#define HMS_MQTT_CLIENT_H

#include <mosquitto.h>
#include <stdbool.h>

#define MQTT_BROKER "139.185.38.211"
#define MQTT_PORT 1883
#define MQTT_USER "mqttuser"
#define MQTT_PASS "123456"
#define MQTT_KEEPALIVE 60

#define HMS_CMD_TOPIC    "hms/cmd"
#define HMS_STATUS_TOPIC "hms/status"

/* Exactly-once delivery, both directions. One constant so the two ends cannot
 * drift apart -- the GUI uses the same value.
 *
 * The cost is real: QoS 2 is a four-part handshake per message, so on the
 * board's ~145ms link to the broker each publish costs roughly half a second
 * of round trips rather than none. It buys no duplicated commands and no lost
 * results, which for start/kill/ota is worth more than the latency. */
#define HMS_MQTT_QOS 2

typedef void (*hms_cmd_callback_t)(void *userdata, const char *cmd);

typedef struct hms_mqtt {
    struct mosquitto *mosq;
    hms_cmd_callback_t cmd_callback;
    void *userdata;
    volatile bool connected;
    volatile bool connecting;
    volatile bool reconnect_enabled;
} hms_mqtt_t;

int hms_mqtt_init(hms_mqtt_t *m, hms_cmd_callback_t cb, void *userdata);
int hms_mqtt_connect(hms_mqtt_t *m);
void hms_mqtt_disconnect(hms_mqtt_t *m);
void hms_mqtt_ensure_connected(hms_mqtt_t *m);
int hms_mqtt_publish(hms_mqtt_t *m, const char *topic, const char *payload);
int hms_mqtt_publish_status(hms_mqtt_t *m, const char *payload);
bool hms_mqtt_connected(const hms_mqtt_t *m);

#endif
