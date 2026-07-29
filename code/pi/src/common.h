/* =========================================================================
 *  Smart Guardian System -- shared declarations
 *  Embedded Systems final project -- Mahan Majlesi (402170516)
 * ========================================================================= */
#ifndef GUARDIAN_COMMON_H
#define GUARDIAN_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define GUARDIAN_VERSION "1.0.0"

/* Monotonic seconds, for intervals and debouncing. */
double now_mono(void);
/* Wall-clock seconds since the epoch, for timestamps we publish. */
double now_wall(void);
/* ISO-8601 UTC timestamp with milliseconds, e.g. 2026-07-27T04:12:33.482Z */
void iso8601_utc(double wall, char *out, size_t outsz);
/* Local-time timestamp, for human-facing email bodies. */
void iso8601_local(double wall, char *out, size_t outsz);

#endif /* GUARDIAN_COMMON_H */
