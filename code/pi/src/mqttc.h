/* =========================================================================
 *  MQTT client (part 3-c) -- libmosquitto, in C, on the board.
 *
 *  Topic tree mandated by the brief, with <student_id> = 402170516:
 *
 *      telemetry/<student_id>/home   CPU temp, memory, load        (periodic)
 *      persons/<student_id>/home     person count + timestamp      (on change)
 *      alarm/<student_id>/home       guard-mode intrusion          (part 4-1)
 *      status/<student_id>/home      online / offline              (LWT)
 *
 *  Everything is published at QoS 1 so the broker acknowledges each message.
 *  The Last Will and Testament is registered before connecting and is
 *  retained, so the laptop learns that the board died even if it subscribes
 *  after the fact -- which is exactly what experiment 3-4 checks.
 * ========================================================================= */
#ifndef GUARDIAN_MQTTC_H
#define GUARDIAN_MQTTC_H

#include <stdbool.h>

bool mqttc_start(void);
void mqttc_stop(void);

/* Publishes the current /proc + /sys telemetry snapshot. */
void mqttc_publish_telemetry(void);

/* Publishes a person-count change. */
void mqttc_publish_persons(int persons, double ts_wall, double fps);

/* Publishes an intrusion alarm (guard mode only). */
void mqttc_publish_alarm(int persons, double ts_wall, double cpu_temp_c);

/* Publishes an arbitrary JSON document to a project topic suffix, used by
 * the watchdog and thermal governor for their own events. */
void mqttc_publish_event(const char *event, const char *detail_json);

bool mqttc_connected(void);

#endif /* GUARDIAN_MQTTC_H */
