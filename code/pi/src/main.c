/* =========================================================================
 *  Smart Guardian System -- main daemon
 *  Embedded Systems final project -- Mahan Majlesi (402170516)
 *
 *  Every piece of application logic the project brief asks for lives in this
 *  C program: the web server and its TLS, the whole REST surface, reading the
 *  board's temperature and memory straight from /proc and /sys, the MQTT
 *  client, the SMTP alerting, the SQLite black box, the software watchdog and
 *  the adaptive thermal governor.
 *
 *  Two satellite processes exist, both of which the brief explicitly permits:
 *
 *    guardian-vision.py   OpenCV person detection (the only Python allowed to
 *                         carry logic), handing annotated frames to this
 *                         process through shared memory.
 *
 *    guardian_api.py      FastAPI, purely to render Swagger UI and forward
 *                         calls to the loopback listener below.
 * ========================================================================= */
#include "api.h"
#include "common.h"
#include "config.h"
#include "events.h"
#include "httpd.h"
#include "log.h"
#include "mailer.h"
#include "mqttc.h"
#include "sdctl.h"
#include "shmframe.h"
#include "state.h"
#include "store.h"
#include "sysinfo.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t STOP = 0;

static void on_signal(int sig)
{
    STOP = sig;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Smart Guardian System " GUARDIAN_VERSION "\n"
            "usage: %s [-c /etc/guardian/guardian.conf] [-v] [-f]\n"
            "  -c PATH  configuration file\n"
            "  -v       verbose (debug) logging\n"
            "  -f       run in the foreground even without systemd\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *conf = "/etc/guardian/guardian.conf";
    log_level_t level = LOG_L_INFO;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      conf = argv[++i];
        else if (!strcmp(argv[i], "-v"))                 level = LOG_L_DEBUG;
        else if (!strcmp(argv[i], "-f"))                 /* default anyway */;
        else { usage(argv[0]); return 2; }
    }

    log_init(level);
    LOGI("Smart Guardian System " GUARDIAN_VERSION " starting (pid %d)", getpid());

    /* SIGPIPE would otherwise kill the process when an HTTP client walks away
     * mid-stream, or when the SMTP connection drops under us. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!config_load(conf)) {
        LOGE("configuration is incomplete -- refusing to start");
        return 1;
    }
    config_log_summary();

    state_init();
    sysinfo_init();

    /* The C daemon owns the shared segment: it is created here and the
     * detector attaches to it, so a detector restart never loses the mapping
     * for the HTTP clients currently streaming. */
    if (!shmframe_open(true)) {
        LOGE("cannot create the shared frame segment");
        return 1;
    }

    if (!store_open(g_cfg.db_path, g_cfg.db_ring_size)) {
        LOGE("cannot open the black-box database");
        shmframe_close();
        return 1;
    }

    if (!mailer_start()) {
        LOGE("cannot start the mail worker");
        store_close();
        shmframe_close();
        return 1;
    }

    /* MQTT is allowed to fail: the broker lives on a separate machine and the
     * board must keep guarding (and keep serving the dashboard) while the
     * laptop is off. libmosquitto reconnects in the background. */
    if (!mqttc_start())
        LOGW("MQTT client did not start; continuing without telemetry publishing");

    if (!httpd_start()) {
        LOGE("cannot start the web server");
        mqttc_stop();
        mailer_stop();
        store_close();
        shmframe_close();
        return 1;
    }

    if (!events_start()) {
        LOGE("cannot start the worker threads");
        httpd_stop();
        mqttc_stop();
        mailer_stop();
        store_close();
        shmframe_close();
        return 1;
    }

    /* Type=notify: only now is the service genuinely available, which is what
     * makes `After=guardian.service` meaningful for the FastAPI unit. */
    char ready[160];
    snprintf(ready, sizeof ready, "serving https on :%d, http :%d redirects",
             g_cfg.https_port, g_cfg.http_port);
    sdctl_notify_ready(ready);
    LOGI("startup complete");

    while (!STOP)
        pause();

    LOGI("signal %d received, shutting down", (int)STOP);
    sdctl_notify_stopping();

    events_stop();
    httpd_stop();
    mqttc_stop();
    mailer_stop();
    store_close();
    shmframe_close();

    LOGI("shutdown complete");
    return 0;
}
