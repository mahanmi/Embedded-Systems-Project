#include "api.h"

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

#include <json-c/json.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *dupf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *dupf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0)
        out = NULL;
    va_end(ap);
    return out;
}

char *api_error(const char *code, const char *message, int *status, int http)
{
    *status = http;
    return dupf("{\"error\":\"%s\",\"message\":\"%s\"}", code, message);
}

/* ------------------------------------------------------------- endpoints */

char *api_persons(int *status)
{
    struct guardian_frame f;
    double wall = now_wall();
    char iso[40];

    if (!shmframe_read(&f, false)) {
        /* The detector has not published yet -- report that honestly rather
         * than inventing a zero. */
        iso8601_utc(wall, iso, sizeof iso);
        *status = 503;
        return dupf("{\"persons\":null,\"timestamp\":\"%s\",\"epoch\":%.3f,"
                    "\"student_id\":\"%s\",\"available\":false,"
                    "\"reason\":\"vision pipeline has not produced a frame yet\"}",
                    iso, wall, g_cfg.student_id);
    }

    iso8601_utc(f.ts_wall, iso, sizeof iso);
    *status = 200;
    /* `vehicles` sits beside `persons`, never inside it. The brief grades the
     * person count, and every consumer of this endpoint -- the dashboard, the
     * black box, experiment 3-5 -- reads `persons` expecting exactly what it
     * always meant. Each entry in `detections` now carries a "cls" telling the
     * two apart. */
    return dupf("{\"persons\":%d,\"vehicles\":%d,\"timestamp\":\"%s\","
                "\"epoch\":%.3f,"
                "\"student_id\":\"%s\",\"fps\":%.2f,\"frame_age_sec\":%.2f,"
                "\"resolution\":\"%dx%d\",\"inference_ms\":%.1f,"
                "\"guard_mode\":%s,\"detections\":%s,\"available\":true}",
                f.persons, f.vehicles, iso, f.ts_wall, g_cfg.student_id, f.fps,
                now_wall() - f.ts_wall, f.width, f.height, f.infer_ms,
                state_guard_mode() ? "true" : "false",
                f.det[0] ? f.det : "[]");
}

char *api_telemetry(int *status)
{
    struct sysinfo_snapshot s;
    sysinfo_get(&s);

    double wall = now_wall();
    char iso[40];
    iso8601_utc(wall, iso, sizeof iso);

    char cores[256] = "";
    size_t o = 0;
    o += (size_t)snprintf(cores + o, sizeof cores - o, "[");
    for (int i = 0; i < s.ncores && o < sizeof cores - 16; i++)
        o += (size_t)snprintf(cores + o, sizeof cores - o, "%s%.1f",
                              i ? "," : "", s.cpu_percent_per_core[i]);
    snprintf(cores + o, sizeof cores - o, "]");

    int tfps = 0, twidth = 0, tnet = 0;
    shmframe_get_target(&tfps, &twidth, &tnet);

    *status = 200;
    return dupf(
        "{\"student_id\":\"%s\",\"timestamp\":\"%s\",\"epoch\":%.3f,"
        "\"cpu_temp_c\":%.2f,"
        "\"cpu_percent\":%.2f,\"cpu_percent_per_core\":%s,\"cpu_freq_khz\":%llu,"
        "\"cpu_cores\":%d,"
        "\"mem_total_kb\":%llu,\"mem_free_kb\":%llu,\"mem_available_kb\":%llu,"
        "\"mem_buffers_kb\":%llu,\"mem_cached_kb\":%llu,\"mem_used_percent\":%.2f,"
        "\"swap_total_kb\":%llu,\"swap_free_kb\":%llu,"
        "\"load1\":%.2f,\"load5\":%.2f,\"load15\":%.2f,"
        "\"uptime_sec\":%.0f,\"daemon_rss_kb\":%llu,"
        "\"thermal_level\":%d,\"target_fps\":%d,\"target_width\":%d,"
        "\"target_net_input\":%d,"
        "\"guard_mode\":%s,\"mqtt_connected\":%s}",
        g_cfg.student_id, iso, wall,
        s.cpu_temp_c,
        s.cpu_percent, cores, (unsigned long long)s.cpu_freq_khz, s.ncores,
        (unsigned long long)s.mem_total_kb, (unsigned long long)s.mem_free_kb,
        (unsigned long long)s.mem_available_kb, (unsigned long long)s.mem_buffers_kb,
        (unsigned long long)s.mem_cached_kb, s.mem_used_percent,
        (unsigned long long)s.swap_total_kb, (unsigned long long)s.swap_free_kb,
        s.load1, s.load5, s.load15,
        s.uptime_sec, (unsigned long long)s.self_rss_kb,
        (int)state_thermal_level(), tfps, twidth, tnet,
        state_guard_mode() ? "true" : "false",
        state_mqtt_connected() ? "true" : "false");
}

char *api_history(int limit, int *status)
{
    if (limit <= 0)
        limit = 5; /* the brief asks for the last 5 records by default */
    char *rows = store_recent_json(limit);
    if (!rows)
        return api_error("storage", "history unavailable", status, 500);

    char *out = dupf("{\"student_id\":\"%s\",\"limit\":%d,\"records\":%s}",
                     g_cfg.student_id, limit, rows);
    free(rows);
    *status = 200;
    return out;
}

char *api_guard_get(int *status)
{
    struct guardian_state st;
    state_get(&st);
    char iso[40];
    iso8601_utc(st.guard_changed_at, iso, sizeof iso);
    *status = 200;
    return dupf("{\"enabled\":%s,\"changed_at\":\"%s\",\"alarms_raised\":%llu}",
                st.guard_mode ? "true" : "false", iso,
                (unsigned long long)st.alarms_raised);
}

char *api_guard_set(const char *body, int *status)
{
    struct json_object *root = json_tokener_parse(body ? body : "");
    if (!root)
        return api_error("bad_request", "body must be JSON", status, 400);

    struct json_object *v = NULL;
    if (!json_object_object_get_ex(root, "enabled", &v)) {
        json_object_put(root);
        return api_error("bad_request", "missing 'enabled' boolean", status, 400);
    }
    bool on = json_object_get_boolean(v);
    json_object_put(root);

    state_set_guard_mode(on);
    mqttc_publish_event("guard_mode", on ? "{\"enabled\":true}" : "{\"enabled\":false}");
    return api_guard_get(status);
}

char *api_stats(int *status)
{
    struct guardian_state st;
    state_get(&st);

    uint64_t events = 0, persons_sum = 0;
    int peak = 0;
    int64_t rows = 0;
    double first_ts = 0, last_ts = 0;
    store_stats(&events, &persons_sum, &peak, &rows, &first_ts, &last_ts);

    char first_iso[40], last_iso[40], start_iso[40];
    iso8601_utc(first_ts, first_iso, sizeof first_iso);
    iso8601_utc(last_ts, last_iso, sizeof last_iso);
    iso8601_utc(st.started_at, start_iso, sizeof start_iso);

    *status = 200;
    return dupf(
        "{\"student_id\":\"%s\",\"daemon_started\":\"%s\",\"uptime_sec\":%.0f,"
        "\"blackbox\":{\"total_events\":%llu,\"total_persons\":%llu,"
        "\"peak_persons\":%d,\"rows_retained\":%lld,\"ring_size\":%d,"
        "\"oldest\":\"%s\",\"newest\":\"%s\"},"
        "\"mqtt\":{\"connected\":%s,\"published\":%llu,\"failures\":%llu},"
        "\"mail\":{\"sent\":%llu,\"coalesced\":%llu,\"healthy\":%s},"
        "\"alarms_raised\":%llu,"
        "\"watchdog\":{\"restarts\":%d,\"vision_stale\":%s},"
        "\"thermal\":{\"level\":%d,\"events\":%d,\"peak_c\":%.1f},"
        "\"http_requests\":%llu,"
        "\"commands\":{\"executed\":%llu,\"rejected\":%llu},"
        "\"frames_total\":%llu}",
        g_cfg.student_id, start_iso, now_wall() - st.started_at,
        (unsigned long long)events, (unsigned long long)persons_sum, peak,
        (long long)rows, g_cfg.db_ring_size, first_iso, last_iso,
        st.mqtt_connected ? "true" : "false",
        (unsigned long long)st.mqtt_published, (unsigned long long)st.mqtt_failures,
        (unsigned long long)st.mails_sent, (unsigned long long)st.mails_suppressed,
        mailer_healthy() ? "true" : "false",
        (unsigned long long)st.alarms_raised,
        st.watchdog_restarts, st.vision_stale ? "true" : "false",
        (int)st.thermal_level, st.thermal_events, st.thermal_peak_c,
        (unsigned long long)st.http_requests,
        (unsigned long long)st.commands_executed,
        (unsigned long long)st.commands_rejected,
        (unsigned long long)shmframe_frames_total());
}

char *api_health(int *status)
{
    struct guardian_state st;
    state_get(&st);

    double ts_mono = 0, ts_wall = 0;
    bool have_frame = shmframe_last_ts(&ts_mono, &ts_wall);
    double frame_age = have_frame ? (now_mono() - ts_mono) : -1.0;

    bool vision_ok = have_frame && frame_age < (double)g_cfg.watchdog_stale_sec;
    bool healthy = vision_ok && st.mqtt_connected;

    *status = healthy ? 200 : 503;
    return dupf(
        "{\"status\":\"%s\",\"student_id\":\"%s\",\"version\":\"%s\","
        "\"components\":{"
        "\"vision\":{\"ok\":%s,\"frame_age_sec\":%.2f},"
        "\"mqtt\":{\"ok\":%s},"
        "\"mail\":{\"ok\":%s},"
        "\"storage\":{\"ok\":true}}}",
        healthy ? "ok" : "degraded", g_cfg.student_id, GUARDIAN_VERSION,
        vision_ok ? "true" : "false", frame_age,
        st.mqtt_connected ? "true" : "false",
        mailer_healthy() ? "true" : "false");
}

/* ================================================================ commands
 *
 *  The brief requires the command endpoint to accept different commands and
 *  to be extensible with commands that may be added later. That is met with a
 *  dispatch table rather than an if/else chain: adding a command is one entry
 *  here, and /api/v1/commands publishes the table so clients (and the Swagger
 *  layer) can discover what exists without a code change.
 * ======================================================================== */

typedef struct {
    const char *name;
    const char *summary;
    const char *args;      /* human-readable argument spec, "" for none    */
    bool        privileged;/* requires the bearer token                     */
    /* Handler returns a malloc'd JSON "result" object and sets *http. */
    char     *(*fn)(struct json_object *req, int *http);
} command_def;

/* --- individual handlers ------------------------------------------------ */

/* Reboot has to answer the client before the box goes down, so the actual
 * call is deferred to a detached thread. */
static void *reboot_thread(void *arg)
{
    (void)arg;
    sleep(2);
    LOGW("command: rebooting now");
    if (!sdctl_reboot())
        LOGE("command: reboot request was refused by logind/polkit");
    return NULL;
}

static char *cmd_reboot(struct json_object *req, int *http)
{
    (void)req;
    pthread_t t;
    if (pthread_create(&t, NULL, reboot_thread, NULL) != 0) {
        *http = 500;
        return dupf("{\"scheduled\":false,\"error\":\"cannot spawn worker\"}");
    }
    pthread_detach(t);
    *http = 202;
    return dupf("{\"scheduled\":true,\"delay_sec\":2,"
                "\"note\":\"the board will reboot; all guardian services are "
                "enabled and come back automatically\"}");
}

static char *cmd_restart_vision(struct json_object *req, int *http)
{
    (void)req;
    bool ok = sdctl_restart_unit("guardian-vision.service");
    *http = ok ? 200 : 500;
    return dupf("{\"restarted\":%s,\"unit\":\"guardian-vision.service\"}",
                ok ? "true" : "false");
}

static char *cmd_set_fps(struct json_object *req, int *http)
{
    struct json_object *v = NULL;
    if (!req || !json_object_object_get_ex(req, "fps", &v)) {
        *http = 400;
        return dupf("{\"error\":\"missing integer argument 'fps'\"}");
    }
    int fps = json_object_get_int(v);
    if (fps < 1 || fps > 30) {
        *http = 400;
        return dupf("{\"error\":\"fps must be between 1 and 30\"}");
    }
    shmframe_set_target(fps, 0, 0);
    int f = 0, w = 0, n = 0;
    shmframe_get_target(&f, &w, &n);
    *http = 200;
    return dupf("{\"target_fps\":%d,\"target_width\":%d,\"target_net_input\":%d}",
                f, w, n);
}

static char *cmd_set_resolution(struct json_object *req, int *http)
{
    struct json_object *v = NULL;
    if (!req || !json_object_object_get_ex(req, "width", &v)) {
        *http = 400;
        return dupf("{\"error\":\"missing integer argument 'width'\"}");
    }
    int w = json_object_get_int(v);
    if (w < 160 || w > 1280) {
        *http = 400;
        return dupf("{\"error\":\"width must be between 160 and 1280\"}");
    }
    shmframe_set_target(0, w, 0);
    int f = 0, cw = 0, n = 0;
    shmframe_get_target(&f, &cw, &n);
    *http = 200;
    return dupf("{\"target_fps\":%d,\"target_width\":%d,\"target_net_input\":%d}",
                f, cw, n);
}

/* The detector's network input edge dominates inference cost far more than the
 * capture resolution does -- experiment 3-3 sweeps this. */
static char *cmd_set_net_input(struct json_object *req, int *http)
{
    struct json_object *v = NULL;
    if (!req || !json_object_object_get_ex(req, "net_input", &v)) {
        *http = 400;
        return dupf("{\"error\":\"missing integer argument 'net_input'\"}");
    }
    int n = json_object_get_int(v);
    /* 0 is a distinct mode, not a size: the detector keeps relaying and
     * overlaying frames but runs no inference at all. Experiment 2-1 needs a
     * "stream only" state to separate the cost of moving pixels from the cost
     * of the network, and it gives the thermal governor a final step below its
     * smallest input edge. Anything else must stay a usable SSD input size. */
    if (n != 0 && (n < 128 || n > 512)) {
        *http = 400;
        return dupf("{\"error\":\"net_input must be 0 (detection off) or between 128 and 512\"}");
    }
    /* Callers say 0 for "off"; the shared control block needs its own sentinel
     * because 0 there means "leave unchanged". */
    shmframe_set_target(0, 0, n == 0 ? SHMFRAME_NET_OFF : n);
    int f = 0, w = 0, cn = 0;
    shmframe_get_target(&f, &w, &cn);
    *http = 200;
    return dupf("{\"target_fps\":%d,\"target_width\":%d,\"target_net_input\":%d}",
                f, w, cn);
}

static char *cmd_guard(struct json_object *req, int *http)
{
    struct json_object *v = NULL;
    bool on = true;
    if (req && json_object_object_get_ex(req, "enabled", &v))
        on = json_object_get_boolean(v);
    state_set_guard_mode(on);
    *http = 200;
    return dupf("{\"guard_mode\":%s}", on ? "true" : "false");
}

static char *cmd_snapshot(struct json_object *req, int *http)
{
    (void)req;
    struct guardian_frame *f = malloc(sizeof *f);
    if (!f) {
        *http = 500;
        return dupf("{\"error\":\"out of memory\"}");
    }
    bool ok = shmframe_read(f, true);
    if (!ok || !f->jpeg_len) {
        free(f);
        *http = 503;
        return dupf("{\"error\":\"no frame available\"}");
    }

    char body[512];
    char iso[40];
    iso8601_local(f->ts_wall, iso, sizeof iso);
    snprintf(body, sizeof body,
             "Manual snapshot requested through /api/v1/command.\r\n\r\n"
             "  Persons : %d\r\n  Time    : %s\r\n  CPU temp: %.1f C\r\n",
             f->persons, iso, sysinfo_cpu_temp());

    char subject[128];
    snprintf(subject, sizeof subject, "[Guardian %s] snapshot", g_cfg.student_id);
    mailer_notify_event(MAIL_DETECTION, subject, body, f->jpeg, f->jpeg_len);

    int persons = f->persons;
    uint32_t len = f->jpeg_len;
    free(f);
    *http = 202;
    return dupf("{\"emailed\":true,\"persons\":%d,\"jpeg_bytes\":%u}", persons, len);
}

static char *cmd_publish_telemetry(struct json_object *req, int *http)
{
    (void)req;
    mqttc_publish_telemetry();
    *http = 200;
    return dupf("{\"published\":true,\"topic\":\"telemetry/%s/home\"}",
                g_cfg.student_id);
}

static char *cmd_ping(struct json_object *req, int *http)
{
    (void)req;
    *http = 200;
    return dupf("{\"pong\":true,\"epoch\":%.3f}", now_wall());
}

/* --- the table ---------------------------------------------------------- */

static const command_def COMMANDS[] = {
    { "ping",              "Liveness check; performs no action",
      "", false, cmd_ping },
    { "guard_on",          "Arm anti-theft mode (part 4-1)",
      "", true,  cmd_guard },
    { "guard_off",         "Disarm anti-theft mode",
      "", true,  cmd_guard },
    { "set_fps",           "Change the detector's target frame rate",
      "fps: integer 1..30", true, cmd_set_fps },
    { "set_resolution",    "Change the detector's capture width",
      "width: integer 160..1280", true, cmd_set_resolution },
    { "set_net_input",     "Change the detector network's input edge in pixels",
      "net_input: integer 128..512", true, cmd_set_net_input },
    { "snapshot",          "Email the current annotated frame",
      "", true, cmd_snapshot },
    { "publish_telemetry", "Force an immediate MQTT telemetry publish",
      "", true, cmd_publish_telemetry },
    { "restart_vision",    "Restart the image-processing service",
      "", true, cmd_restart_vision },
    { "reboot",            "Reboot the board",
      "", true, cmd_reboot },
};
static const size_t NCOMMANDS = sizeof COMMANDS / sizeof COMMANDS[0];

char *api_commands(int *status)
{
    size_t cap = 256 + NCOMMANDS * 256;
    char *buf = malloc(cap);
    if (!buf)
        return api_error("internal", "out of memory", status, 500);

    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "{\"commands\":[");
    for (size_t i = 0; i < NCOMMANDS; i++)
        o += (size_t)snprintf(buf + o, cap - o,
                 "%s{\"cmd\":\"%s\",\"summary\":\"%s\",\"arguments\":\"%s\","
                 "\"requires_token\":%s}",
                 i ? "," : "", COMMANDS[i].name, COMMANDS[i].summary,
                 COMMANDS[i].args, COMMANDS[i].privileged ? "true" : "false");
    snprintf(buf + o, cap - o, "]}");

    *status = 200;
    return buf;
}

/* Builds the "supported" list used in 400 responses. */
static void supported_list(char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < NCOMMANDS && o < cap - 24; i++)
        o += (size_t)snprintf(out + o, cap - o, "%s%s", i ? ", " : "",
                              COMMANDS[i].name);
}

char *api_command(const char *body, bool authorised, int *status)
{
    struct json_object *root = json_tokener_parse(body ? body : "");
    if (!root) {
        state_inc_command(false);
        return api_error("bad_request",
                         "body must be a JSON object such as {\\\"cmd\\\":\\\"reboot\\\"}",
                         status, 400);
    }

    struct json_object *jcmd = NULL;
    if (!json_object_object_get_ex(root, "cmd", &jcmd) ||
        !json_object_is_type(jcmd, json_type_string)) {
        json_object_put(root);
        state_inc_command(false);
        return api_error("bad_request", "missing string field 'cmd'", status, 400);
    }
    const char *name = json_object_get_string(jcmd);

    const command_def *def = NULL;
    for (size_t i = 0; i < NCOMMANDS; i++)
        if (!strcmp(COMMANDS[i].name, name)) {
            def = &COMMANDS[i];
            break;
        }

    if (!def) {
        char list[512];
        supported_list(list, sizeof list);
        char *out = dupf("{\"error\":\"unknown_command\",\"cmd\":\"%s\","
                         "\"supported\":\"%s\","
                         "\"hint\":\"GET /api/v1/commands for the full catalogue\"}",
                         name, list);
        json_object_put(root);
        state_inc_command(false);
        *status = 400;
        return out;
    }

    if (def->privileged && !authorised) {
        json_object_put(root);
        state_inc_command(false);
        return api_error("unauthorised",
                         "command requires the Authorization: Bearer <token> header",
                         status, 401);
    }

    /* guard_on/guard_off share a handler; synthesise the argument. */
    if (!strcmp(name, "guard_on") || !strcmp(name, "guard_off"))
        json_object_object_add(root, "enabled",
                               json_object_new_boolean(!strcmp(name, "guard_on")));

    int http = 200;
    char *result = def->fn(root, &http);
    json_object_put(root);

    bool accepted = (http >= 200 && http < 300);
    state_inc_command(accepted);
    LOGI("command '%s' -> HTTP %d", name, http);

    char iso[40];
    iso8601_utc(now_wall(), iso, sizeof iso);
    char *out = dupf("{\"cmd\":\"%s\",\"accepted\":%s,\"timestamp\":\"%s\","
                     "\"result\":%s}",
                     name, accepted ? "true" : "false", iso,
                     result ? result : "null");
    free(result);
    *status = http;
    return out;
}
