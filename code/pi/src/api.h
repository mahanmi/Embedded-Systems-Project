/* =========================================================================
 *  RESTful API (part 2) -- the response bodies. Transport lives in httpd.c.
 *
 *  Every endpoint the brief mandates is implemented here in C; the FastAPI
 *  process is a documentation and gateway layer that forwards to these same
 *  handlers over the loopback listener, so there is exactly one implementation
 *  of the logic.
 *
 *      GET  /api/v1/stream      MJPEG multipart stream
 *      GET  /api/v1/persons     current person count + timestamp
 *      GET  /api/v1/telemetry   CPU temperature, free memory, CPU load
 *      GET  /api/v1/history     last N detections (default 5)
 *      POST /api/v1/command     {"cmd":"reboot"} and friends
 *
 *  plus the endpoints part 4 needs:
 *
 *      GET  /api/v1/guard       guard-mode state
 *      POST /api/v1/guard       {"enabled":true}
 *      GET  /api/v1/stats       lifetime counters (black box)
 *      GET  /api/v1/health      liveness + component status
 *      GET  /api/v1/commands    self-describing command catalogue
 * ========================================================================= */
#ifndef GUARDIAN_API_H
#define GUARDIAN_API_H

#include <stdbool.h>
#include <stddef.h>

/* Each returns a malloc'd JSON document the caller frees, and sets *status. */
char *api_persons(int *status);
char *api_telemetry(int *status);
char *api_history(int limit, int *status);
char *api_guard_get(int *status);
char *api_guard_set(const char *body, int *status);
char *api_stats(int *status);
char *api_health(int *status);
char *api_commands(int *status);

/* POST /api/v1/command. `authorised` reflects the bearer-token check done by
 * the transport layer. */
char *api_command(const char *body, bool authorised, int *status);

/* Convenience for error bodies. */
char *api_error(const char *code, const char *message, int *status, int http);

#endif /* GUARDIAN_API_H */
