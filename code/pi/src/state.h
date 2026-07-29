/* =========================================================================
 *  Shared runtime state
 *
 *  A single mutex-protected record that every thread reads and a few threads
 *  write: guard mode, the thermal governor's current step, watchdog health,
 *  and the counters the API reports. Keeping it in one place (rather than
 *  scattering atomics through the modules) makes the concurrency story easy
 *  to state: no thread holds this lock while doing I/O.
 * ========================================================================= */
#ifndef GUARDIAN_STATE_H
#define GUARDIAN_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    THERM_NORMAL = 0,   /* full target FPS / resolution      */
    THERM_STEP1  = 1,   /* first reduction                   */
    THERM_STEP2  = 2,   /* second reduction                  */
    THERM_STEP3  = 3,   /* minimum viable pipeline           */
} thermal_level_t;

struct guardian_state {
    /* part 4-1: anti-theft mode */
    bool guard_mode;
    double guard_changed_at;      /* wall clock                            */

    /* part 4-3: software watchdog */
    bool  vision_stale;
    int   watchdog_restarts;
    double last_watchdog_action;  /* monotonic                             */

    /* part 4-4: adaptive thermal management */
    thermal_level_t thermal_level;
    int    thermal_events;
    double thermal_peak_c;

    /* counters */
    uint64_t mqtt_published;
    uint64_t mqtt_failures;
    uint64_t mails_sent;
    uint64_t mails_suppressed;    /* coalesced by the debounce             */
    uint64_t alarms_raised;
    uint64_t http_requests;
    uint64_t commands_executed;
    uint64_t commands_rejected;

    /* last detection seen by the event consumer */
    int    last_persons;
    double last_detection_wall;

    bool   mqtt_connected;
    double started_at;            /* wall clock at daemon start            */
};

void state_init(void);
void state_get(struct guardian_state *out);

bool state_guard_mode(void);
void state_set_guard_mode(bool on);

void state_set_thermal(thermal_level_t lvl, double peak_c);
thermal_level_t state_thermal_level(void);

void state_set_vision_stale(bool stale);
void state_note_watchdog_restart(void);

void state_set_mqtt_connected(bool up);
bool state_mqtt_connected(void);

void state_note_detection(int persons, double wall);

/* Counter bumps -- named rather than a generic enum so call sites read well. */
void state_inc_mqtt_published(void);
void state_inc_mqtt_failure(void);
void state_inc_mail_sent(void);
void state_inc_mail_suppressed(void);
void state_inc_alarm(void);
void state_inc_http(void);
void state_inc_command(bool accepted);

#endif /* GUARDIAN_STATE_H */
