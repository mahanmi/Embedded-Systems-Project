/* =========================================================================
 *  HTTP/HTTPS transport (part 1-a, 1-b)
 *
 *  Three libmicrohttpd daemons run side by side:
 *
 *    :80    plaintext -- answers every request with 301 Moved Permanently to
 *           the https origin, so the page is reachable "only over https" as
 *           the brief requires (experiment 1-4 checks for the 301 exactly).
 *
 *    :443   TLS (MHD_USE_TLS, GnuTLS underneath) with the self-signed
 *           certificate whose CN is the student number. Serves the dashboard
 *           and the whole /api/v1 surface.
 *
 *    :8081  plaintext bound to 127.0.0.1 only -- the loopback interface the
 *           FastAPI documentation layer proxies to. It never leaves the box,
 *           so a second TLS handshake per request would be pure overhead on a
 *           Cortex-A53.
 *
 *  Ports 80 and 443 are privileged; the daemon keeps CAP_NET_BIND_SERVICE as
 *  an ambient capability from systemd and otherwise runs as the unprivileged
 *  `guardian` user.
 * ========================================================================= */
#ifndef GUARDIAN_HTTPD_H
#define GUARDIAN_HTTPD_H

#include <stdbool.h>

bool httpd_start(void);
void httpd_stop(void);

#endif /* GUARDIAN_HTTPD_H */
