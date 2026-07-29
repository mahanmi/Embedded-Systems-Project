/* =========================================================================
 *  Black box (part 4-2)
 *
 *  Detection history in SQLite, capped as a ring buffer so an appliance that
 *  runs for months never fills the SD card. Two design points worth noting:
 *
 *    * The cap is enforced by an AFTER INSERT trigger inside the database
 *      rather than by periodic housekeeping from C, so the invariant holds
 *      even if the daemon is killed mid-write.
 *
 *    * Pruning would destroy the lifetime detection count the brief asks the
 *      API to report, so a separate single-row `stats` table carries a
 *      monotonic counter that pruning never touches.
 * ========================================================================= */
#ifndef GUARDIAN_STORE_H
#define GUARDIAN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct detection_row {
    int64_t id;
    double  ts_wall;
    int     persons;
    double  cpu_temp_c;
    double  fps;
    int     guard_mode;
    int     emailed;
    char    note[64];
};

bool store_open(const char *path, int ring_size);
void store_close(void);

/* Records one detection event and bumps the lifetime counters. */
bool store_add_detection(double ts_wall, int persons, double cpu_temp_c,
                         double fps, bool guard_mode, bool emailed,
                         const char *note);

/* Newest-first. Returns how many rows were written into `out`. */
int store_recent(struct detection_row *out, int max_rows);

/* Lifetime aggregates that survive ring-buffer pruning. */
bool store_stats(uint64_t *events, uint64_t *persons_sum, int *peak_persons,
                 int64_t *rows_now, double *first_ts, double *last_ts);

/* Serialises the newest `limit` rows as a JSON array. Caller frees. */
char *store_recent_json(int limit);

#endif /* GUARDIAN_STORE_H */
