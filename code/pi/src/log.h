/* =========================================================================
 *  Logging -- writes to stderr in the sd-daemon prefix format so journald
 *  assigns each line the right priority (systemd reads the "<N>" prefix).
 * ========================================================================= */
#ifndef GUARDIAN_LOG_H
#define GUARDIAN_LOG_H

typedef enum {
    LOG_L_ERR   = 3,
    LOG_L_WARN  = 4,
    LOG_L_INFO  = 6,
    LOG_L_DEBUG = 7,
} log_level_t;

void log_init(log_level_t min_level);
void log_msg(log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOGE(...) log_msg(LOG_L_ERR,   __VA_ARGS__)
#define LOGW(...) log_msg(LOG_L_WARN,  __VA_ARGS__)
#define LOGI(...) log_msg(LOG_L_INFO,  __VA_ARGS__)
#define LOGD(...) log_msg(LOG_L_DEBUG, __VA_ARGS__)

#endif /* GUARDIAN_LOG_H */
