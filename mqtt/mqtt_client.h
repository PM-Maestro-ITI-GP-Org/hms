#ifndef HMS_MQTT_CLIENT_H
#define HMS_MQTT_CLIENT_H

#include <mosquitto.h>
#include <pthread.h>
#include <stdbool.h>

#define MQTT_BROKER "139.185.38.211"
#define MQTT_PORT 1883
#define MQTT_USER "mqttuser"
#define MQTT_PASS "123456"

/* 15, not 60. This is the backstop for a host that vanishes without warning --
 * a pulled power lead, a kernel panic, a cut network. The broker only decides a
 * client is gone after roughly 1.5x keepalive with nothing heard from it, so 60
 * meant the GUI kept showing a dead board as healthy for about a minute and a
 * half. At 15 the will fires in about 22s.
 *
 * The heartbeat below is what actually makes it feel immediate; this is only
 * what catches the case where the heartbeat and the GUI are both fine but the
 * board is not. */
#define MQTT_KEEPALIVE 15

#define HMS_CMD_TOPIC    "hms/cmd"
#define HMS_STATUS_TOPIC "hms/status"

/* Liveness, published on the normal status topic so the GUI needs no second
 * subscription and dispatches it on "state" like everything else.
 *
 * Retained, so a GUI that starts later learns the host's state from the broker
 * immediately instead of staring at an empty page until the next heartbeat. */
#define HMS_HOST_ONLINE_JSON  "{\"state\":\"host\",\"online\":true}"
#define HMS_HOST_OFFLINE_JSON "{\"state\":\"host\",\"online\":false}"

/* How often the host says it is alive.
 *
 * 1s: fast enough that the GUI can call the host dead after three misses and
 * still be under four seconds, which is what "real time" means to someone
 * watching a screen.
 *
 * Sent at QoS 0 deliberately -- see HMS_MQTT_QOS_BEAT. */
#define HMS_HEARTBEAT_MS 1000

/* Exactly-once delivery for commands, both directions. One constant so the two
 * ends cannot drift apart -- the GUI uses the same value.
 *
 * The cost is real: QoS 2 is a four-part handshake per message, so on the
 * board's ~145ms link to the broker each publish costs roughly half a second
 * of round trips rather than none. It buys no duplicated commands and no lost
 * results, which for start/kill/ota is worth more than the latency. */
#define HMS_MQTT_QOS 2

/* The heartbeat is the one thing that must NOT be QoS 2.
 *
 * At 145ms to the broker a QoS 2 publish costs ~580ms of round trips. A beat
 * every second at that price would spend most of the link on saying "still
 * here", and would make the beat itself the thing that stutters. It is also
 * pointless: a heartbeat is idempotent state, so a lost one is replaced a
 * second later by the next. Losing three in a row is exactly the condition the
 * GUI is watching for anyway. */
#define HMS_MQTT_QOS_BEAT 0

/* Retained state (online/offline) goes at QoS 1: it must not be lost the way a
 * beat may be, but it is published rarely enough that QoS 2's second round trip
 * buys nothing -- a duplicate "online" is indistinguishable from the original. */
#define HMS_MQTT_QOS_STATE 1

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

    /* Heartbeat thread. Deliberately its own thread rather than a tick inside
       the refresh loop: refresh() does ssh work that can take seconds, and a
       beat that waits behind it would go silent exactly when the host is busy
       -- which the GUI would read as the board having died. */
    pthread_t       hb_thread;
    volatile bool   hb_running;
    bool            hb_started;

    /* Delivery tracking, used only to flush the final "offline" on the way
       out. A clean DISCONNECT makes the broker discard the will, so on an
       orderly shutdown that last publish is the only thing that tells the GUI
       anything -- and publishing it without waiting drops it on the floor. */
    pthread_mutex_t pub_lock;
    pthread_cond_t  pub_cv;
    int             pub_wait_mid;
    bool            pub_done;
} hms_mqtt_t;

int hms_mqtt_init(hms_mqtt_t *m, hms_cmd_callback_t cb, void *userdata);
int hms_mqtt_connect(hms_mqtt_t *m);

/* Start the once-a-second "still here" beat. Safe to call more than once.
 * hms_mqtt_disconnect() stops and joins it. */
int hms_mqtt_start_heartbeat(hms_mqtt_t *m);

void hms_mqtt_disconnect(hms_mqtt_t *m);
void hms_mqtt_ensure_connected(hms_mqtt_t *m);
int hms_mqtt_publish(hms_mqtt_t *m, const char *topic, const char *payload);
int hms_mqtt_publish_status(hms_mqtt_t *m, const char *payload);
bool hms_mqtt_connected(const hms_mqtt_t *m);

#endif
