#include "mqttc.h"

#include "common.h"
#include "config.h"
#include "log.h"
#include "shmframe.h"
#include "state.h"
#include "sysinfo.h"

#include <mosquitto.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct mosquitto *M = NULL;

/* --- broker address failover ---------------------------------------------
 * The broker lives on the laptop. Depending on which Wi-Fi that laptop is
 * attached to it is either directly routable or only reachable through an SSH
 * port-forward that lands on this board's loopback. Rather than requiring a
 * config edit when the laptop moves, the client alternates between the two
 * configured addresses whenever it has been disconnected for a while. */
static pthread_t T_SUPERVISOR;
static volatile bool SUP_RUN = false;
static int  ACTIVE_HOST = 0;                 /* 0 = primary, 1 = fallback   */
static double LAST_CONNECT_CHANGE = 0.0;     /* monotonic                    */

static const char *host_for(int idx)
{
    if (idx == 1 && g_cfg.mqtt_host_fallback[0])
        return g_cfg.mqtt_host_fallback;
    return g_cfg.mqtt_host;
}

static char T_TELEMETRY[128];
static char T_PERSONS[128];
static char T_ALARM[128];
static char T_STATUS[128];
static char T_EVENT[128];

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)ud;
    if (rc != 0) {
        /* rc 4/5 are bad-credentials / not-authorised: the anonymous-login
         * lockdown in experiment 3-6 lands here. */
        LOGE("mqtt: connect refused by %s (rc=%d: %s)", host_for(ACTIVE_HOST),
             rc, mosquitto_connack_string(rc));
        state_set_mqtt_connected(false);
        return;
    }
    LOGI("mqtt: connected to %s:%d as '%s'", host_for(ACTIVE_HOST),
         g_cfg.mqtt_port, g_cfg.mqtt_user);
    state_set_mqtt_connected(true);
    LAST_CONNECT_CHANGE = now_mono();

    /* Clear the retained "offline" will with a retained "online". */
    char payload[256];
    char iso[40];
    iso8601_utc(now_wall(), iso, sizeof iso);
    int n = snprintf(payload, sizeof payload,
                     "{\"status\":\"online\",\"student_id\":\"%s\","
                     "\"timestamp\":\"%s\",\"version\":\"%s\"}",
                     g_cfg.student_id, iso, GUARDIAN_VERSION);
    mosquitto_publish(m, NULL, T_STATUS, n, payload, 1, true);
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    bool was_up = state_mqtt_connected();
    state_set_mqtt_connected(false);
    if (was_up)
        LAST_CONNECT_CHANGE = now_mono();
    if (rc != 0)
        LOGW("mqtt: unexpected disconnect from %s (rc=%d) -- will reconnect",
             host_for(ACTIVE_HOST), rc);
    else
        LOGI("mqtt: disconnected");
}

/* Alternates the broker address after a sustained outage. Runs at 1/5 Hz, so
 * it never competes with libmosquitto's own backoff. */
static void *supervisor(void *arg)
{
    (void)arg;
    const double SWITCH_AFTER = 25.0;

    while (SUP_RUN) {
        for (int i = 0; i < 20 && SUP_RUN; i++)
            usleep(250000);
        if (!SUP_RUN)
            break;

        if (state_mqtt_connected() || !g_cfg.mqtt_host_fallback[0])
            continue;
        if (!strcmp(g_cfg.mqtt_host, g_cfg.mqtt_host_fallback))
            continue;
        if (now_mono() - LAST_CONNECT_CHANGE < SWITCH_AFTER)
            continue;

        ACTIVE_HOST ^= 1;
        LAST_CONNECT_CHANGE = now_mono();
        LOGW("mqtt: no broker for %.0fs, trying %s:%d instead", SWITCH_AFTER,
             host_for(ACTIVE_HOST), g_cfg.mqtt_port);

        mosquitto_disconnect(M);
        int rc = mosquitto_connect_async(M, host_for(ACTIVE_HOST),
                                         g_cfg.mqtt_port, g_cfg.mqtt_keepalive);
        if (rc != MOSQ_ERR_SUCCESS)
            LOGW("mqtt: %s is not reachable either (%s)", host_for(ACTIVE_HOST),
                 mosquitto_strerror(rc));
    }
    return NULL;
}

static void on_publish(struct mosquitto *m, void *ud, int mid)
{
    (void)m; (void)ud; (void)mid;
    state_inc_mqtt_published();
}

static void on_log(struct mosquitto *m, void *ud, int level, const char *str)
{
    (void)m; (void)ud;
    if (level == MOSQ_LOG_ERR || level == MOSQ_LOG_WARNING)
        LOGW("mqtt: %s", str);
}

static void pub(const char *topic, const char *payload, bool retain)
{
    if (!M)
        return;
    int rc = mosquitto_publish(M, NULL, topic, (int)strlen(payload), payload,
                               1 /* QoS 1, as the brief requires */, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        state_inc_mqtt_failure();
        /* Not fatal: mosquitto_loop_start() keeps retrying the connection and
         * this message is simply lost, which is the honest behaviour to show
         * in experiment 2-4 (network cable pulled mid-stream). */
        LOGW("mqtt: publish to %s failed: %s", topic, mosquitto_strerror(rc));
    }
}

bool mqttc_start(void)
{
    mosquitto_lib_init();

    snprintf(T_TELEMETRY, sizeof T_TELEMETRY, "telemetry/%s/home", g_cfg.student_id);
    snprintf(T_PERSONS,   sizeof T_PERSONS,   "persons/%s/home",   g_cfg.student_id);
    snprintf(T_ALARM,     sizeof T_ALARM,     "alarm/%s/home",     g_cfg.student_id);
    snprintf(T_STATUS,    sizeof T_STATUS,    "status/%s/home",    g_cfg.student_id);
    snprintf(T_EVENT,     sizeof T_EVENT,     "event/%s/home",     g_cfg.student_id);

    char client_id[64];
    snprintf(client_id, sizeof client_id, "guardian-%s", g_cfg.student_id);

    M = mosquitto_new(client_id, true /* clean session */, NULL);
    if (!M) {
        LOGE("mqtt: mosquitto_new failed");
        return false;
    }

    mosquitto_connect_callback_set(M, on_connect);
    mosquitto_disconnect_callback_set(M, on_disconnect);
    mosquitto_publish_callback_set(M, on_publish);
    mosquitto_log_callback_set(M, on_log);

    /* Credentials come from the environment; the broker denies anonymous
     * connections (experiment 3-6). */
    if (mosquitto_username_pw_set(M, g_cfg.mqtt_user, g_cfg.mqtt_pass)
        != MOSQ_ERR_SUCCESS) {
        LOGE("mqtt: cannot set credentials");
        return false;
    }

    /* --- Last Will and Testament, registered BEFORE connecting --- */
    char will[256];
    int wn = snprintf(will, sizeof will,
                      "{\"status\":\"offline\",\"student_id\":\"%s\","
                      "\"reason\":\"unexpected disconnect (LWT)\"}",
                      g_cfg.student_id);
    if (mosquitto_will_set(M, T_STATUS, wn, will, 1 /* QoS 1 */, true /* retained */)
        != MOSQ_ERR_SUCCESS) {
        LOGE("mqtt: cannot register LWT");
        return false;
    }

    /* Reconnect with exponential backoff, capped -- experiments 2-4 and 3-4
     * both cut the link and expect the board to come back on its own. */
    mosquitto_reconnect_delay_set(M, 1, 30, true);

    ACTIVE_HOST = 0;
    LAST_CONNECT_CHANGE = now_mono();
    int rc = mosquitto_connect_async(M, host_for(ACTIVE_HOST), g_cfg.mqtt_port,
                                     g_cfg.mqtt_keepalive);
    if (rc != MOSQ_ERR_SUCCESS)
        LOGW("mqtt: initial connect to %s:%d deferred (%s)",
             host_for(ACTIVE_HOST), g_cfg.mqtt_port, mosquitto_strerror(rc));

    if (mosquitto_loop_start(M) != MOSQ_ERR_SUCCESS) {
        LOGE("mqtt: cannot start network loop");
        return false;
    }

    SUP_RUN = true;
    if (pthread_create(&T_SUPERVISOR, NULL, supervisor, NULL) != 0) {
        LOGW("mqtt: broker failover supervisor could not start");
        SUP_RUN = false;
    }

    LOGI("mqtt: client '%s' started, LWT on %s (QoS 1, retained)", client_id,
         T_STATUS);
    return true;
}

void mqttc_stop(void)
{
    if (!M)
        return;

    if (SUP_RUN) {
        SUP_RUN = false;
        pthread_join(T_SUPERVISOR, NULL);
    }

    /* A clean shutdown is NOT a will condition: say so explicitly, otherwise
     * the retained "online" would linger after a deliberate stop. */
    char payload[256];
    char iso[40];
    iso8601_utc(now_wall(), iso, sizeof iso);
    snprintf(payload, sizeof payload,
             "{\"status\":\"offline\",\"student_id\":\"%s\",\"timestamp\":\"%s\","
             "\"reason\":\"clean shutdown\"}", g_cfg.student_id, iso);
    pub(T_STATUS, payload, true);

    mosquitto_disconnect(M);
    mosquitto_loop_stop(M, false);
    mosquitto_destroy(M);
    M = NULL;
    mosquitto_lib_cleanup();
    LOGI("mqtt: stopped");
}

void mqttc_publish_telemetry(void)
{
    struct sysinfo_snapshot s;
    sysinfo_get(&s);

    double wall = now_wall();
    char iso[40];
    iso8601_utc(wall, iso, sizeof iso);

    char payload[640];
    snprintf(payload, sizeof payload,
        "{\"student_id\":\"%s\",\"timestamp\":\"%s\",\"epoch\":%.3f,"
        "\"cpu_temp_c\":%.2f,\"cpu_percent\":%.2f,\"cpu_freq_khz\":%llu,"
        "\"mem_total_kb\":%llu,\"mem_free_kb\":%llu,\"mem_available_kb\":%llu,"
        "\"mem_used_percent\":%.2f,\"load1\":%.2f,\"uptime_sec\":%.0f,"
        "\"persons\":%d,\"guard_mode\":%s,\"thermal_level\":%d}",
        g_cfg.student_id, iso, wall, s.cpu_temp_c, s.cpu_percent,
        (unsigned long long)s.cpu_freq_khz, (unsigned long long)s.mem_total_kb,
        (unsigned long long)s.mem_free_kb, (unsigned long long)s.mem_available_kb,
        s.mem_used_percent, s.load1, s.uptime_sec,
        shmframe_persons(), state_guard_mode() ? "true" : "false",
        (int)state_thermal_level());

    pub(T_TELEMETRY, payload, false);
}

void mqttc_publish_persons(int persons, int vehicles, double ts_wall, double fps)
{
    char iso[40];
    iso8601_utc(ts_wall, iso, sizeof iso);

    /* `epoch` is the capture instant taken from CLOCK_REALTIME inside the
     * detector. Experiment 3-5 subtracts it from the laptop's receive time to
     * get the end-to-end delay, so it must be the capture time and not the
     * publish time.
     *
     * `vehicles` is appended rather than inserted, and the topic is unchanged,
     * so anything already subscribed keeps parsing this payload exactly as
     * before. Nothing downstream treats it as a trigger. */
    char payload[384];
    snprintf(payload, sizeof payload,
        "{\"student_id\":\"%s\",\"persons\":%d,\"timestamp\":\"%s\","
        "\"epoch\":%.3f,\"fps\":%.2f,\"cpu_temp_c\":%.2f,\"guard_mode\":%s,"
        "\"vehicles\":%d}",
        g_cfg.student_id, persons, iso, ts_wall, fps, sysinfo_cpu_temp(),
        state_guard_mode() ? "true" : "false", vehicles);

    pub(T_PERSONS, payload, false);
}

void mqttc_publish_alarm(int persons, double ts_wall, double cpu_temp_c)
{
    char iso[40];
    iso8601_utc(ts_wall, iso, sizeof iso);

    char payload[384];
    snprintf(payload, sizeof payload,
        "{\"student_id\":\"%s\",\"alarm\":\"intrusion\",\"persons\":%d,"
        "\"timestamp\":\"%s\",\"epoch\":%.3f,\"cpu_temp_c\":%.2f,"
        "\"severity\":\"critical\"}",
        g_cfg.student_id, persons, iso, ts_wall, cpu_temp_c);

    pub(T_ALARM, payload, false);
    state_inc_alarm();
    LOGW("ALARM published: %d person(s) while guard mode is armed", persons);
}

void mqttc_publish_event(const char *event, const char *detail_json)
{
    char iso[40];
    iso8601_utc(now_wall(), iso, sizeof iso);

    char payload[512];
    snprintf(payload, sizeof payload,
        "{\"student_id\":\"%s\",\"event\":\"%s\",\"timestamp\":\"%s\","
        "\"detail\":%s}",
        g_cfg.student_id, event, iso, detail_json ? detail_json : "null");

    pub(T_EVENT, payload, false);
}

bool mqttc_connected(void) { return state_mqtt_connected(); }
