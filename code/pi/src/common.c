#include "common.h"

#include <stdio.h>

double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double now_wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void stamp(double wall, char *out, size_t outsz, bool utc)
{
    time_t secs = (time_t)wall;
    long ms = (long)((wall - (double)secs) * 1000.0);
    if (ms < 0) ms = 0;
    if (ms > 999) ms = 999;

    struct tm tm;
    if (utc)
        gmtime_r(&secs, &tm);
    else
        localtime_r(&secs, &tm);

    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out, outsz, "%s.%03ld%s", base, ms, utc ? "Z" : "");
}

void iso8601_utc(double wall, char *out, size_t outsz)   { stamp(wall, out, outsz, true); }
void iso8601_local(double wall, char *out, size_t outsz) { stamp(wall, out, outsz, false); }
