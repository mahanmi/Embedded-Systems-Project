#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static log_level_t g_min = LOG_L_INFO;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_init(log_level_t min_level) { g_min = min_level; }

void log_msg(log_level_t lvl, const char *fmt, ...)
{
    if (lvl > g_min)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_lock);
    /* The "<N>" prefix is consumed by journald and mapped to a syslog
     * priority; when run from a terminal it is simply visible noise. */
    fprintf(stderr, "<%d>%s.%03ld ", (int)lvl, stamp, ts.tv_nsec / 1000000L);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_lock);
    va_end(ap);
}
