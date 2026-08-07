/*
 * mqtt_client.c
 * MQTT service for HMS using libmosquitto.
 * Subscribes to hms/cmd, publishes responses on hms/status.
 */
#include "mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_message_cb(struct mosquitto *mosq, void *userdata,
                          const struct mosquitto_message *msg)
{
    (void)mosq;
    hms_mqtt_t *m = (hms_mqtt_t *)userdata;
    const char *payload = (const char *)msg->payload;
    int payload_len = msg->payloadlen;

    char cmd[2048];
    int n = (payload && payload_len > 0) ? payload_len : 0;
    if (n >= (int)sizeof(cmd)) n = (int)sizeof(cmd) - 1;
    if (n > 0)
        memcpy(cmd, payload, n);
    cmd[n] = '\0';

    printf("[MQTT] Received cmd on %s: %s\n", msg->topic, cmd);

    if (m->cmd_callback)
        m->cmd_callback(m->userdata, cmd);
}

static void on_connect_cb(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    hms_mqtt_t *m = (hms_mqtt_t *)userdata;

    if (rc == 0) {
        m->connected = true;
        m->connecting = false;
        printf("[MQTT] Connected to broker %s:%d\n", MQTT_BROKER, MQTT_PORT);
        mosquitto_subscribe(m->mosq, NULL, HMS_CMD_TOPIC, 0);
        printf("[MQTT] Subscribed to %s\n", HMS_CMD_TOPIC);
    } else {
        m->connected = false;
        fprintf(stderr, "[MQTT] Connect failed (rc=%d, %s)\n",
                rc, mosquitto_strerror(rc));
    }
}

static void on_disconnect_cb(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    hms_mqtt_t *m = (hms_mqtt_t *)userdata;
    m->connected = false;
    m->connecting = false;
    fprintf(stderr, "[MQTT] Disconnected (rc=%d) — will retry\n", rc);
}

int hms_mqtt_init(hms_mqtt_t *m, hms_cmd_callback_t cb, void *userdata)
{
    memset(m, 0, sizeof(*m));
    m->cmd_callback = cb;
    m->userdata = userdata;

    /* libmosquitto requires this before any other call. It was missing
       entirely; what it initialises (the RNG behind message ids, and the
       library's own threading state) then depended on whatever the process
       happened to have set up. */
    mosquitto_lib_init();

    /* The client id was the fixed string "hms". Two HMS instances -- an old
       one that has not noticed the network dropped yet and the one that just
       started -- present the same id, and the broker disconnects whichever
       connected first. The pair then take turns evicting each other and
       neither stays up long enough to answer a command. */
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "hms_%d", (int)getpid());

    m->mosq = mosquitto_new(client_id, true, m);
    if (!m->mosq) {
        fprintf(stderr, "[MQTT] Failed to create mosquitto instance\n");
        mosquitto_lib_cleanup();
        return -1;
    }

    mosquitto_username_pw_set(m->mosq, MQTT_USER, MQTT_PASS);
    mosquitto_connect_callback_set(m->mosq, on_connect_cb);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect_cb);
    mosquitto_message_callback_set(m->mosq, on_message_cb);
    mosquitto_reconnect_delay_set(m->mosq, 1, 30, true);

    m->connected = false;
    m->reconnect_enabled = true;

    return 0;
}

int hms_mqtt_connect(hms_mqtt_t *m)
{
    int rc;

    m->connected = false;
    m->connecting = false;

    rc = mosquitto_loop_start(m->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] Failed to start network thread: %s\n",
                mosquitto_strerror(rc));
        return -1;
    }

    rc = mosquitto_connect_async(m->mosq, MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        /* Transient failure (e.g. network not up yet): the loop thread is
           running, so hms_mqtt_ensure_connected() retries from main. */
        fprintf(stderr, "[MQTT] Initial connect failed (%s) — will retry\n",
                mosquitto_strerror(rc));
        return 0;
    }

    m->connecting = true;
    printf("[MQTT] Connecting to %s:%d (async, will retry until connected)\n",
           MQTT_BROKER, MQTT_PORT);
    return 0;
}

void hms_mqtt_ensure_connected(hms_mqtt_t *m)
{
    int rc;

    if (!m->mosq || m->connected || m->connecting || !m->reconnect_enabled)
        return;

    m->connecting = true;
    rc = mosquitto_reconnect_async(m->mosq);
    if (rc == MOSQ_ERR_SUCCESS || rc == MOSQ_ERR_CONN_PENDING) {
        printf("[MQTT] Retrying connection to %s:%d\n", MQTT_BROKER, MQTT_PORT);
        return;
    }

    m->connecting = false;
    if (rc != MOSQ_ERR_NO_CONN)
        fprintf(stderr, "[MQTT] Reconnect attempt failed: %s\n",
                mosquitto_strerror(rc));
}

void hms_mqtt_disconnect(hms_mqtt_t *m)
{
    m->reconnect_enabled = false;
    m->connected = false;
    m->connecting = false;
    if (m->mosq) {
        /* Disconnect before stopping the loop: stopping it first leaves the
           broker to time the session out on keepalive instead of seeing a
           clean DISCONNECT. */
        mosquitto_disconnect(m->mosq);
        mosquitto_loop_stop(m->mosq, false);
        mosquitto_destroy(m->mosq);
        m->mosq = NULL;
        mosquitto_lib_cleanup();
    }
}

int hms_mqtt_publish(hms_mqtt_t *m, const char *topic, const char *payload)
{
    if (!m->mosq || !m->connected) return -1;
    size_t len = strlen(payload);
    int rc = mosquitto_publish(m->mosq, NULL, topic, len, payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] publish to %s failed: %s\n",
                topic, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

int hms_mqtt_publish_status(hms_mqtt_t *m, const char *payload)
{
    return hms_mqtt_publish(m, HMS_STATUS_TOPIC, payload);
}

bool hms_mqtt_connected(const hms_mqtt_t *m)
{
    return m->connected;
}
