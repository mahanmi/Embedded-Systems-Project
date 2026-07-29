/* =========================================================================
 *  Configuration
 *
 *  Two sources, deliberately separated:
 *
 *    * /etc/guardian/guardian.conf -- non-secret settings, world-readable by
 *      the guardian group, shipped in the repository.
 *
 *    * the process environment -- every credential, injected by systemd from
 *      /etc/guardian/secrets.env (root:guardian 0640, never in the repo).
 *      The project brief forbids hard-coded passwords and API keys, so the
 *      daemon refuses to start when a required secret is absent rather than
 *      falling back to a compiled-in default.
 * ========================================================================= */
#ifndef GUARDIAN_CONFIG_H
#define GUARDIAN_CONFIG_H

#include <stdbool.h>

struct guardian_config {
    /* identity */
    char student_id[32];
    char student_name[128];

    /* web */
    int  http_port;        /* plaintext listener; only ever 301-redirects   */
    int  https_port;       /* TLS listener                                  */
    int  loopback_port;    /* 127.0.0.1 plaintext, for the FastAPI gateway  */
    char public_host[128]; /* used to build the redirect Location header    */
    char tls_cert[256];
    char tls_key[256];

    /* mqtt */
    char mqtt_host[128];
    /* Second address to try when the primary is unreachable. The broker runs
     * on the laptop, whose Wi-Fi sometimes lands on a different subnet behind
     * a NAT; an SSH port-forward then presents the same broker on the board's
     * loopback. Failing over between the two keeps the board publishing
     * without anyone editing a config file mid-demo. */
    char mqtt_host_fallback[128];
    int  mqtt_port;
    char mqtt_user[64];
    int  mqtt_keepalive;
    int  mqtt_telemetry_interval;

    /* mail */
    char smtp_url[160];
    char smtp_user[128];
    char mail_from[128];
    char mail_to[128];
    int  mail_debounce_sec;
    int  mail_guard_debounce_sec;

    /* storage */
    char db_path[256];
    int  db_ring_size;

    /* vision defaults */
    int  target_fps;
    int  target_width;
    int  target_net_input;  /* detector input edge, px */

    /* part 4 */
    int    watchdog_stale_sec;
    double thermal_high_c;
    double thermal_low_c;
    bool   guard_default;

    /* secrets -- populated from the environment, never from the file */
    const char *smtp_pass;
    const char *mqtt_pass;
    const char *api_token;
};

extern struct guardian_config g_cfg;

/* Load defaults, overlay the file, then pull secrets from the environment.
 * Returns false (with a specific error logged) if a secret is missing. */
bool config_load(const char *path);

/* Redact secrets from a dump of the effective configuration -- used by the
 * /api/v1/health endpoint and at startup. */
void config_log_summary(void);

#endif /* GUARDIAN_CONFIG_H */
