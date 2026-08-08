/*
 * mqtt_client.c
 * MQTT service for HMS using libmosquitto.
 * Subscribes to hms/cmd, publishes responses on hms/status.
 */
#include "mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Publish the retained online/offline marker.
 *
 * Retained on purpose: it is the answer to "is the board there?", and a GUI
 * that connects a minute from now deserves that answer from the broker rather
 * than having to wait for the next beat. Each publish overwrites the last, so
 * the broker holds exactly one, always the current one.
 *
 * wait_ms > 0 blocks until the broker has acknowledged it. Only the shutdown
 * path needs that; everything else would rather not stall. */
static int publish_host_state(hms_mqtt_t *m, bool online, int wait_ms)
{
    if (!m->mosq) return -1;

    const char *payload = online ? HMS_HOST_ONLINE_JSON : HMS_HOST_OFFLINE_JSON;
    int mid = 0;

    if (wait_ms > 0) {
        pthread_mutex_lock(&m->pub_lock);
        m->pub_done = false;
        m->pub_wait_mid = -1;
        pthread_mutex_unlock(&m->pub_lock);
    }

    int rc = mosquitto_publish(m->mosq, &mid, HMS_STATUS_TOPIC,
                               (int)strlen(payload), payload,
                               HMS_MQTT_QOS_STATE, true /* retain */);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] host-state publish failed: %s\n",
                mosquitto_strerror(rc));
        return -1;
    }
    if (wait_ms <= 0) return 0;

    /* Wait for the broker's PUBACK, but never longer than wait_ms -- shutdown
       must not hang because the network went away first. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += wait_ms / 1000;
    deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&m->pub_lock);
    m->pub_wait_mid = mid;
    while (!m->pub_done) {
        if (pthread_cond_timedwait(&m->pub_cv, &m->pub_lock, &deadline) != 0)
            break;
    }
    bool done = m->pub_done;
    pthread_mutex_unlock(&m->pub_lock);

    if (!done)
        fprintf(stderr, "[MQTT] final offline marker not acknowledged in %dms "
                        "(the will covers it)\n", wait_ms);
    return done ? 0 : -1;
}

static void on_publish_cb(struct mosquitto *mosq, void *userdata, int mid)
{
    (void)mosq;
    hms_mqtt_t *m = (hms_mqtt_t *)userdata;

    pthread_mutex_lock(&m->pub_lock);
    if (m->pub_wait_mid == mid) {
        m->pub_done = true;
        pthread_cond_broadcast(&m->pub_cv);
    }
    pthread_mutex_unlock(&m->pub_lock);
}

/*
 * The beat. Nothing but "still here", once a second, at QoS 0.
 *
 * It carries a monotonically increasing counter so the GUI can tell a fresh
 * beat from a retained message replayed at subscribe time, and so a gap is
 * visible in a log after the fact rather than only in the moment.
 */
static void *heartbeat_thread(void *arg)
{
    hms_mqtt_t *m = (hms_mqtt_t *)arg;
    unsigned long beat = 0;

    while (m->hb_running) {
        if (m->connected && m->mosq) {
            char payload[128];
            snprintf(payload, sizeof(payload),
                     "{\"state\":\"host\",\"online\":true,\"beat\":%lu}", beat++);
            /* Errors are ignored on purpose: a dropped beat is what the GUI's
               watchdog already handles, and logging one per second on a flaky
               link would bury everything else in the console. */
            (void)mosquitto_publish(m->mosq, NULL, HMS_STATUS_TOPIC,
                                    (int)strlen(payload), payload,
                                    HMS_MQTT_QOS_BEAT, false);
        }

        struct timespec ts = {
            HMS_HEARTBEAT_MS / 1000,
            (long)(HMS_HEARTBEAT_MS % 1000) * 1000000L
        };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

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
        mosquitto_subscribe(m->mosq, NULL, HMS_CMD_TOPIC, HMS_MQTT_QOS);
        printf("[MQTT] Subscribed to %s\n", HMS_CMD_TOPIC);

        /* Overwrite whatever the broker is still holding -- most likely the
           "offline" left by the will after the last ungraceful exit. Published
           from here rather than after connect() returns because a reconnect
           takes this path too, and a reconnected host that never says it is
           back leaves the GUI showing it as dead. */
        publish_host_state(m, true, 0);
        printf("[MQTT] Announced host online (retained), beat every %dms\n",
               HMS_HEARTBEAT_MS);
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

    pthread_mutex_init(&m->pub_lock, NULL);
    pthread_cond_init(&m->pub_cv, NULL);

    /*
     * The will: what the broker publishes on HMS's behalf if HMS stops talking
     * without saying goodbye -- a pulled power lead, a panic, a severed link.
     *
     * This is the whole answer to "the GUI does not notice when I switch the
     * board off". Nothing on the host can report its own sudden death, so the
     * broker has to do it, and it only will if it was told what to say while
     * the connection was still healthy. Hence: before connect, not after.
     *
     * Retained, so it also fixes the case where the board dies while the GUI is
     * closed and the GUI is opened afterwards -- it gets "offline" the moment
     * it subscribes, rather than an empty screen.
     *
     * Note the broker DISCARDS the will on a clean DISCONNECT, which is why
     * hms_mqtt_disconnect() publishes the offline marker by hand as well.
     */
    int rc = mosquitto_will_set(m->mosq, HMS_STATUS_TOPIC,
                                (int)strlen(HMS_HOST_OFFLINE_JSON),
                                HMS_HOST_OFFLINE_JSON,
                                HMS_MQTT_QOS_STATE, true /* retain */);
    if (rc != MOSQ_ERR_SUCCESS)
        fprintf(stderr, "[MQTT] WARNING: could not set will (%s) — a sudden "
                        "host death will go unreported\n",
                mosquitto_strerror(rc));

    mosquitto_username_pw_set(m->mosq, MQTT_USER, MQTT_PASS);
    mosquitto_connect_callback_set(m->mosq, on_connect_cb);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect_cb);
    mosquitto_message_callback_set(m->mosq, on_message_cb);
    mosquitto_publish_callback_set(m->mosq, on_publish_cb);
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

int hms_mqtt_start_heartbeat(hms_mqtt_t *m)
{
    if (m->hb_started) return 0;

    m->hb_running = true;
    if (pthread_create(&m->hb_thread, NULL, heartbeat_thread, m) != 0) {
        m->hb_running = false;
        fprintf(stderr, "[MQTT] WARNING: heartbeat thread failed to start — "
                        "the GUI will fall back to the %ds keepalive\n",
                MQTT_KEEPALIVE);
        return -1;
    }
    m->hb_started = true;
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

    /* Stop the beat before announcing the shutdown, or the thread can publish
       another "online" after the offline marker and leave the GUI convinced
       the host is still up. Joined, not just flagged: it may be mid-publish. */
    if (m->hb_started) {
        m->hb_running = false;
        pthread_join(m->hb_thread, NULL);
        m->hb_started = false;
    }

    /* Say goodbye properly while still connected. The will does not cover this
       path -- the broker throws it away when a client disconnects cleanly -- so
       without this an orderly `slay hms` would leave the last retained message
       saying "online" forever, and the GUI would believe it. */
    if (m->mosq && m->connected) {
        printf("[MQTT] Announcing host offline ...\n");
        publish_host_state(m, false, 2000);
    }

    m->connected = false;
    m->connecting = false;
    if (m->mosq) {
        /* Disconnect first so the broker sees a clean DISCONNECT rather than
           timing the session out on keepalive.

           The loop is still stopped with force=true. force=false joins the
           network thread, and that thread only returns once it notices the
           disconnect -- which it never does if HMS was killed while the broker
           was unreachable and the thread is sitting in a reconnect backoff.
           Hanging on Ctrl-C is a worse failure than a cancelled thread in a
           process that is about to exit anyway. */
        mosquitto_disconnect(m->mosq);
        mosquitto_loop_stop(m->mosq, true);
        mosquitto_destroy(m->mosq);
        m->mosq = NULL;
        mosquitto_lib_cleanup();
    }
}

int hms_mqtt_publish(hms_mqtt_t *m, const char *topic, const char *payload)
{
    if (!m->mosq || !m->connected) return -1;
    size_t len = strlen(payload);
    int rc = mosquitto_publish(m->mosq, NULL, topic, len, payload,
                               HMS_MQTT_QOS, false);
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
