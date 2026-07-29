#include "events.h"

#include "common.h"
#include "config.h"
#include "log.h"
#include "mailer.h"
#include "mqttc.h"
#include "sdctl.h"
#include "shmframe.h"
#include "state.h"
#include "store.h"
#include "sysinfo.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool RUN = false;
static pthread_t T_TELEMETRY, T_DETECT, T_SUPERVISOR;

/* Sleep in short slices so shutdown is prompt. */
static void nap(double seconds)
{
    const double slice = 0.25;
    for (double t = 0; RUN && t < seconds; t += slice)
        usleep((useconds_t)(slice * 1e6));
}

/* ===================================================== telemetry loop ==== */

static void *telemetry_loop(void *arg)
{
    (void)arg;
    double last_publish = 0.0;

    while (RUN) {
        sysinfo_sample();

        double now = now_mono();
        if (now - last_publish >= (double)g_cfg.mqtt_telemetry_interval) {
            mqttc_publish_telemetry();
            last_publish = now;
        }
        nap(1.0);
    }
    return NULL;
}

/* ====================================================== detection loop ====
 *
 *  An "event" is a change in the person count, not every frame: at 3 FPS with
 *  someone standing in view, per-frame reporting would mean 180 MQTT messages
 *  and 180 database rows a minute for a single continuous presence. The
 *  transitions that matter are:
 *
 *      0 -> N     someone arrived
 *      N -> M>N   more people arrived
 *      N -> 0     the scene emptied  (recorded, but not emailed)
 *
 *  A short confirmation window suppresses single-frame flicker from the
 *  detector, which MobileNet-SSD produces regularly at low light.
 */

static void *detect_loop(void *arg)
{
    (void)arg;

    int  reported = 0;            /* the count we last acted on              */
    int  candidate = -1;          /* count awaiting confirmation             */
    int  candidate_hits = 0;
    double last_zero_report = 0.0;

    /* Two consecutive agreeing frames before we believe a change. At the low
     * frame rates this board achieves that costs a few hundred milliseconds
     * and removes almost all of the flicker. */
    const int CONFIRM_FRAMES = 2;

    struct guardian_frame *f = malloc(sizeof *f);
    if (!f) {
        LOGE("detect: out of memory");
        return NULL;
    }
    double last_seen_ts = 0.0;

    while (RUN) {
        double ts = 0;
        if (!shmframe_last_ts(&ts, NULL) || ts <= last_seen_ts) {
            nap(0.1);
            continue;
        }

        if (!shmframe_read(f, true)) {
            nap(0.1);
            continue;
        }
        last_seen_ts = f->ts_mono;

        /* Placeholder frames carry no scene information; reacting to their
         * "0 people" would fabricate a departure event every time the capture
         * host hiccups. */
        if (f->flags & GUARDIAN_FRAME_SYNTHETIC)
            continue;

        int persons = f->persons;

        /* --- confirmation window --- */
        if (persons != reported) {
            if (persons == candidate) {
                candidate_hits++;
            } else {
                candidate = persons;
                candidate_hits = 1;
            }
            if (candidate_hits < CONFIRM_FRAMES)
                continue;
        } else {
            candidate = -1;
            candidate_hits = 0;
            /* Unchanged and non-zero: nothing to announce. The periodic
             * telemetry publish already carries the current count. */
            continue;
        }

        /* --- confirmed transition --- */
        int prev = reported;
        reported = persons;
        candidate = -1;
        candidate_hits = 0;

        double temp = sysinfo_cpu_temp();
        bool guard = state_guard_mode();
        state_note_detection(persons, f->ts_wall);

        /* Person count always goes out on MQTT, including the drop to zero,
         * so a subscriber can reconstruct occupancy. */
        mqttc_publish_persons(persons, f->ts_wall, f->fps);

        if (persons == 0) {
            /* Record the scene emptying at most once a minute so an empty
             * corridor with a flickering detector cannot fill the ring. */
            if (now_mono() - last_zero_report > 60.0) {
                store_add_detection(f->ts_wall, 0, temp, f->fps, guard, false,
                                    "scene cleared");
                last_zero_report = now_mono();
            }
            LOGI("detection: scene cleared (was %d)", prev);
            continue;
        }

        bool rising = (persons > prev);
        LOGI("detection: %d person(s) [was %d]%s", persons, prev,
             guard ? " GUARD ARMED" : "");

        /* Part 4-1: in guard mode every detection is an alarm. */
        if (guard) {
            mqttc_publish_alarm(persons, f->ts_wall, temp);

            char body[1024], subject[160], iso[40];
            iso8601_local(f->ts_wall, iso, sizeof iso);
            snprintf(subject, sizeof subject,
                     "*** INTRUSION *** [Guardian %s] %d person(s)",
                     g_cfg.student_id, persons);
            snprintf(body, sizeof body,
                     "GUARD MODE ALARM\r\n"
                     "================\r\n\r\n"
                     "  Persons        : %d\r\n"
                     "  Time           : %s\r\n"
                     "  CPU temperature: %.1f C\r\n"
                     "  MQTT topic     : alarm/%s/home\r\n\r\n"
                     "Anti-theft mode is armed, so this alert was sent "
                     "immediately rather than waiting for the %d s detection "
                     "debounce.\r\n",
                     persons, iso, temp, g_cfg.student_id,
                     g_cfg.mail_debounce_sec);
            mailer_notify_event(MAIL_ALARM, subject, body, f->jpeg, f->jpeg_len);
        }

        /* Part 3-b: the ordinary (debounced) detection email. Only on a
         * rising edge -- a count going 3 -> 2 is not news. */
        bool mailed = false;
        if (rising)
            mailed = mailer_notify_detection(persons, f->ts_wall, temp, f->jpeg,
                                             f->jpeg_len, guard);

        /* Part 4-2: black box. `mailed` records what actually happened, so the
         * history distinguishes an event that produced its own email from one
         * that the debounce folded into a later message. */
        store_add_detection(f->ts_wall, persons, temp, f->fps, guard, mailed,
                            guard ? "guard alarm"
                                  : (rising ? (mailed ? "detection, emailed"
                                                      : "detection, coalesced")
                                            : "count decreased"));
    }

    free(f);
    return NULL;
}

/* ==================================================== supervisor loop ====
 *  part 4-3 software watchdog + part 4-4 adaptive thermal management
 */

/* Thermal ladder. Index 0 is the configured target; each step trades frame
 * rate and resolution for die temperature. */
static const struct { int fps; int width; int net; const char *label; } LADDER[] = {
    { 10, 640, 300, "normal" },
    {  5, 480, 256, "step 1" },
    {  3, 320, 224, "step 2" },
    {  2, 320, 192, "step 3" },
};

static void apply_thermal(thermal_level_t lvl, double temp)
{
    int fps = LADDER[lvl].fps;
    int w   = LADDER[lvl].width;
    int net = LADDER[lvl].net;

    /* Level 0 restores whatever the configuration asked for, which may differ
     * from the ladder's nominal top entry. */
    if (lvl == THERM_NORMAL) {
        fps = g_cfg.target_fps;
        w   = g_cfg.target_width;
        net = g_cfg.target_net_input;
    }

    shmframe_set_target(fps, w, net);
    state_set_thermal(lvl, temp);

    char detail[192];
    snprintf(detail, sizeof detail,
             "{\"level\":%d,\"label\":\"%s\",\"cpu_temp_c\":%.1f,"
             "\"target_fps\":%d,\"target_width\":%d,\"net_input\":%d}",
             (int)lvl, LADDER[lvl].label, temp, fps, w, net);
    mqttc_publish_event("thermal", detail);

    char subject[160], body[768];
    snprintf(subject, sizeof subject, "[Guardian %s] thermal %s at %.1f C",
             g_cfg.student_id,
             lvl == THERM_NORMAL ? "recovered" : "throttling", temp);
    snprintf(body, sizeof body,
             "Adaptive thermal management\r\n"
             "===========================\r\n\r\n"
             "  CPU temperature : %.1f C\r\n"
             "  High threshold  : %.1f C\r\n"
             "  Low  threshold  : %.1f C (hysteresis)\r\n"
             "  New level       : %d (%s)\r\n"
             "  Target FPS      : %d\r\n"
             "  Capture width   : %d px\r\n"
             "  Network input   : %d px\r\n\r\n"
             "%s\r\n",
             temp, g_cfg.thermal_high_c, g_cfg.thermal_low_c,
             (int)lvl, LADDER[lvl].label, fps, w, net,
             lvl == THERM_NORMAL
                 ? "The board cooled below the low threshold, so the detector "
                   "has been returned to its configured frame rate and "
                   "resolution."
                 : "The board crossed the high threshold, so the detector's "
                   "frame rate and capture resolution were reduced to shed "
                   "load.");
    mailer_notify_event(MAIL_THERMAL, subject, body, NULL, 0);

    LOGW("thermal: level %d (%s) at %.1f C -> %d fps, %d px capture, "
         "%d px network", (int)lvl, LADDER[lvl].label, temp, fps, w, net);
}

static void *supervisor_loop(void *arg)
{
    (void)arg;

    double last_restart = 0.0;
    int    consecutive_restarts = 0;
    bool   was_stale = false;

    /* Give the detector a grace period after our own start-up: loading the
     * Caffe model on a Pi 3B+ takes a few seconds. */
    double grace_until = now_mono() + 45.0;

    while (RUN) {
        nap(5.0);
        if (!RUN)
            break;

        /* ---------------- part 4-3: software watchdog ----------------
         * Staleness is measured against the last frame that carried real
         * capture input. The detector keeps publishing a "no signal"
         * placeholder so the dashboard and MJPEG stream stay alive, and
         * counting those as fresh would hide exactly the fault this watchdog
         * exists to catch. */
        double ts_mono = 0;
        bool have = shmframe_last_real_ts(&ts_mono);
        double age = have ? (now_mono() - ts_mono) : -1.0;
        bool stale = have ? (age > (double)g_cfg.watchdog_stale_sec)
                          : (now_mono() > grace_until);

        if (stale && now_mono() > grace_until) {
            state_set_vision_stale(true);

            /* Restart-storm guard: never more often than once a minute, and
             * stop trying after five consecutive failures so a permanently
             * unplugged camera does not thrash the board. */
            double since = now_mono() - last_restart;
            if (since > 60.0 && consecutive_restarts < 5) {
                if (have)
                    LOGW("watchdog: no real frame for %.0f s -- restarting the "
                         "detector", age);
                else
                    LOGW("watchdog: the capture host has never connected -- "
                         "restarting the detector");

                char body[900], subject[160];
                snprintf(subject, sizeof subject,
                         "[Guardian %s] CAMERA TAMPERING suspected",
                         g_cfg.student_id);
                snprintf(body, sizeof body,
                         "Software watchdog\r\n"
                         "=================\r\n\r\n"
                         "The image-processing service has not produced a new "
                         "frame for %.0f seconds (threshold %d s).\r\n\r\n"
                         "  Possible causes : camera unplugged or covered, the\r\n"
                         "                    upstream stream stopped, or the\r\n"
                         "                    detector process hung.\r\n"
                         "  Action taken    : restarting guardian-vision.service\r\n"
                         "  Restart number  : %d\r\n\r\n"
                         "If frames do not resume, the hardware feeding the "
                         "board should be inspected.\r\n",
                         age, g_cfg.watchdog_stale_sec, consecutive_restarts + 1);
                mailer_notify_event(MAIL_WATCHDOG, subject, body, NULL, 0);

                char detail[160];
                snprintf(detail, sizeof detail,
                         "{\"frame_age_sec\":%.0f,\"threshold_sec\":%d,"
                         "\"action\":\"restart guardian-vision.service\"}",
                         age, g_cfg.watchdog_stale_sec);
                mqttc_publish_event("watchdog", detail);

                if (sdctl_restart_unit("guardian-vision.service")) {
                    state_note_watchdog_restart();
                    consecutive_restarts++;
                } else {
                    LOGE("watchdog: restart request refused -- check the polkit rule");
                }
                last_restart = now_mono();
                grace_until = now_mono() + 45.0; /* let it load the model */
            } else if (consecutive_restarts >= 5) {
                LOGE("watchdog: giving up after %d restarts; manual "
                     "intervention required", consecutive_restarts);
            }
            was_stale = true;
        } else if (!stale) {
            if (was_stale) {
                LOGI("watchdog: frames resumed after %d restart(s)",
                     consecutive_restarts);
                mqttc_publish_event("watchdog", "{\"recovered\":true}");
            }
            state_set_vision_stale(false);
            consecutive_restarts = 0;
            was_stale = false;
        }

        /* ------------ part 4-4: adaptive thermal management ------------ */
        double temp = sysinfo_cpu_temp();
        if (temp > 0) {
            thermal_level_t cur = state_thermal_level();
            thermal_level_t next = cur;

            if (temp >= g_cfg.thermal_high_c && cur < THERM_STEP3)
                next = (thermal_level_t)(cur + 1);      /* step down harder */
            else if (temp <= g_cfg.thermal_low_c && cur > THERM_NORMAL)
                next = (thermal_level_t)(cur - 1);      /* recover a step   */

            if (next != cur)
                apply_thermal(next, temp);
            else
                state_set_thermal(cur, temp); /* keeps the peak-temp record */
        }
    }
    return NULL;
}

/* ============================================================== lifecycle */

bool events_start(void)
{
    RUN = true;

    /* Publish the configured defaults so the detector knows what to aim for
     * before the governor has anything to say. */
    shmframe_set_target(g_cfg.target_fps, g_cfg.target_width,
                        g_cfg.target_net_input);

    if (pthread_create(&T_TELEMETRY, NULL, telemetry_loop, NULL) != 0 ||
        pthread_create(&T_DETECT, NULL, detect_loop, NULL) != 0 ||
        pthread_create(&T_SUPERVISOR, NULL, supervisor_loop, NULL) != 0) {
        LOGE("events: cannot start worker threads");
        RUN = false;
        return false;
    }
    LOGI("events: telemetry, detection and supervisor threads running");
    return true;
}

void events_stop(void)
{
    if (!RUN)
        return;
    RUN = false;
    pthread_join(T_TELEMETRY, NULL);
    pthread_join(T_DETECT, NULL);
    pthread_join(T_SUPERVISOR, NULL);
    LOGI("events: workers stopped");
}
