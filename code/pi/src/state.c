#include "state.h"

#include "common.h"
#include "config.h"
#include "log.h"

#include <pthread.h>
#include <string.h>

static struct guardian_state S;
static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;

#define WITH_LOCK(body)                 \
    do {                                \
        pthread_mutex_lock(&L);         \
        body;                           \
        pthread_mutex_unlock(&L);       \
    } while (0)

void state_init(void)
{
    pthread_mutex_lock(&L);
    memset(&S, 0, sizeof S);
    S.guard_mode    = g_cfg.guard_default;
    S.thermal_level = THERM_NORMAL;
    S.last_persons  = -1;
    S.started_at    = now_wall();
    S.guard_changed_at = S.started_at;
    pthread_mutex_unlock(&L);
}

void state_get(struct guardian_state *out) { WITH_LOCK(*out = S); }

bool state_guard_mode(void)
{
    bool v;
    WITH_LOCK(v = S.guard_mode);
    return v;
}

void state_set_guard_mode(bool on)
{
    bool changed = false;
    pthread_mutex_lock(&L);
    if (S.guard_mode != on) {
        S.guard_mode = on;
        S.guard_changed_at = now_wall();
        changed = true;
    }
    pthread_mutex_unlock(&L);
    if (changed)
        LOGI("guard mode %s", on ? "ARMED" : "disarmed");
}

void state_set_thermal(thermal_level_t lvl, double peak_c)
{
    pthread_mutex_lock(&L);
    if (S.thermal_level != lvl) {
        S.thermal_level = lvl;
        S.thermal_events++;
    }
    if (peak_c > S.thermal_peak_c)
        S.thermal_peak_c = peak_c;
    pthread_mutex_unlock(&L);
}

thermal_level_t state_thermal_level(void)
{
    thermal_level_t v;
    WITH_LOCK(v = S.thermal_level);
    return v;
}

void state_set_vision_stale(bool stale)
{
    bool changed = false;
    pthread_mutex_lock(&L);
    if (S.vision_stale != stale) {
        S.vision_stale = stale;
        changed = true;
    }
    pthread_mutex_unlock(&L);
    if (changed)
        LOGW("vision pipeline %s", stale ? "STALE" : "healthy again");
}

void state_note_watchdog_restart(void)
{
    WITH_LOCK(S.watchdog_restarts++; S.last_watchdog_action = now_mono());
}

void state_set_mqtt_connected(bool up) { WITH_LOCK(S.mqtt_connected = up); }

bool state_mqtt_connected(void)
{
    bool v;
    WITH_LOCK(v = S.mqtt_connected);
    return v;
}

void state_note_detection(int persons, double wall)
{
    WITH_LOCK(S.last_persons = persons; S.last_detection_wall = wall);
}

void state_inc_mqtt_published(void)  { WITH_LOCK(S.mqtt_published++); }
void state_inc_mqtt_failure(void)    { WITH_LOCK(S.mqtt_failures++); }
void state_inc_mail_sent(void)       { WITH_LOCK(S.mails_sent++); }
void state_inc_mail_suppressed(void) { WITH_LOCK(S.mails_suppressed++); }
void state_inc_telegram_sent(void)       { WITH_LOCK(S.telegrams_sent++); }
void state_inc_telegram_suppressed(void) { WITH_LOCK(S.telegrams_suppressed++); }
void state_inc_alarm(void)           { WITH_LOCK(S.alarms_raised++); }
void state_inc_http(void)            { WITH_LOCK(S.http_requests++); }

void state_inc_command(bool accepted)
{
    WITH_LOCK(if (accepted) S.commands_executed++; else S.commands_rejected++);
}
