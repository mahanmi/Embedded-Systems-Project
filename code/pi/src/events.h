/* =========================================================================
 *  Background workers
 *
 *  Three loops, each on its own thread, that turn observations into actions:
 *
 *    telemetry  1 Hz /proc + /sys sampling, periodic MQTT telemetry publish
 *
 *    detector   watches the shared-memory frame sequence and reacts to
 *               person-count transitions: black-box row, MQTT persons
 *               publish, email (debounced), and -- in guard mode -- the
 *               alarm topic. (parts 3-b, 3-c, 4-1, 4-2)
 *
 *    supervisor the software watchdog (part 4-3) and the adaptive thermal
 *               governor (part 4-4), both of which tick at 5 s.
 * ========================================================================= */
#ifndef GUARDIAN_EVENTS_H
#define GUARDIAN_EVENTS_H

#include <stdbool.h>

bool events_start(void);
void events_stop(void);

#endif /* GUARDIAN_EVENTS_H */
