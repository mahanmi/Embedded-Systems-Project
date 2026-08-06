#include "config.h"

#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct guardian_config g_cfg;

static void set_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);

    set_str(g_cfg.student_id, sizeof g_cfg.student_id, "402170516");
    set_str(g_cfg.student_name, sizeof g_cfg.student_name, "Mahan Majlesi");

    g_cfg.http_port     = 80;
    g_cfg.https_port    = 443;
    g_cfg.loopback_port = 8081;
    /* Bonjour name, not a literal IP, for the same reason mqtt_host below is
     * one -- this board is a DHCP client and its address moves. It also matches
     * the certificate's SAN list without a regeneration. See guardian.conf. */
    set_str(g_cfg.public_host, sizeof g_cfg.public_host, "mahan.local");
    set_str(g_cfg.tls_cert, sizeof g_cfg.tls_cert, "/etc/guardian/certs/guardian.crt");
    set_str(g_cfg.tls_key,  sizeof g_cfg.tls_key,  "/etc/guardian/certs/guardian.key");

    /* Bonjour name, not a literal IP: the laptop is a DHCP client and its
     * address moves. See the comment in config/guardian.conf. */
    set_str(g_cfg.mqtt_host, sizeof g_cfg.mqtt_host, "your-laptop.local");
    set_str(g_cfg.mqtt_host_fallback, sizeof g_cfg.mqtt_host_fallback, "127.0.0.1");
    g_cfg.mqtt_port = 1883;
    set_str(g_cfg.mqtt_user, sizeof g_cfg.mqtt_user, "guardian");
    g_cfg.mqtt_keepalive = 20;
    g_cfg.mqtt_telemetry_interval = 5;

    set_str(g_cfg.smtp_url,  sizeof g_cfg.smtp_url,  "smtps://smtp.gmail.com:465");
    set_str(g_cfg.smtp_user, sizeof g_cfg.smtp_user, "you@example.com");
    set_str(g_cfg.mail_from, sizeof g_cfg.mail_from, "you@example.com");
    set_str(g_cfg.mail_to,   sizeof g_cfg.mail_to,   "you@example.com");
    g_cfg.mail_debounce_sec = 30;
    g_cfg.mail_guard_debounce_sec = 3;

    g_cfg.telegram_enabled = true;
    set_str(g_cfg.telegram_api_base, sizeof g_cfg.telegram_api_base,
            "https://api.telegram.org");
    /* Seeded to the mail intervals so the two channels behave identically on
     * day one; they are separate keys so Telegram can be made more responsive
     * later without touching the brief's "one email every 30 seconds". */
    g_cfg.telegram_debounce_sec = 30;
    g_cfg.telegram_guard_debounce_sec = 3;
    g_cfg.telegram_commands_enabled = true;
    /* Long enough that a held-down button cannot make a 600 MHz board encode
     * and upload back to back, short enough that /preview still feels live. */
    g_cfg.telegram_preview_cooldown_sec = 10;

    set_str(g_cfg.db_path, sizeof g_cfg.db_path, "/var/lib/guardian/guardian.db");
    g_cfg.db_ring_size = 1000;

    g_cfg.target_fps       = 15;
    g_cfg.target_width     = 640;
    g_cfg.target_net_input = 300;

    g_cfg.watchdog_stale_sec = 30;
    g_cfg.thermal_high_c = 70.0;
    g_cfg.thermal_low_c  = 65.0;
    g_cfg.guard_default  = false;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

/* Strips a trailing "  # comment" from a value.
 *
 * The '#' must be preceded by whitespace so that a value which legitimately
 * contains one (a URL fragment, say) is left alone. Integer keys survived
 * without this because atoi() stops at the first non-digit, but string keys
 * silently absorbed the comment text -- which is how the Swagger gateway ended
 * up trying to reach "http://127.0.0.1:8081      # 127.0.0.1 only, ...". */
static void strip_inline_comment(char *v)
{
    for (char *p = v + 1; *p; p++) {
        if (*p == '#' && isspace((unsigned char)p[-1])) {
            *p = '\0';
            while (p > v && isspace((unsigned char)p[-1]))
                *--p = '\0';
            return;
        }
    }
}

static bool truthy(const char *v)
{
    return !strcasecmp(v, "1") || !strcasecmp(v, "true") ||
           !strcasecmp(v, "yes") || !strcasecmp(v, "on");
}

/* Keys in guardian.conf that belong to the Python vision service, which parses
 * the same file. They are not ours to interpret, but they are not typos either,
 * and the unknown-key warning below only earns its place if it means something:
 * with these fifteen unlisted every startup logged fifteen warnings, so a real
 * typo arrived as the sixteenth and nobody would ever pick it out.
 *
 * Names, not prefixes. Matching "camera_*" wholesale would swallow "camera_hsot"
 * too, which is exactly the mistake the warning exists to catch. The price is
 * that a new key in vision/guardian_vision.py has to be added here as well --
 * cheap, and it fails loudly (one spurious warning) rather than silently.
 *
 * Keys both sides read -- student_id, target_fps, target_width,
 * target_net_input -- are deliberately absent: apply() consumes those itself. */
static bool key_belongs_to_vision(const char *k)
{
    static const char *vision_keys[] = {
        /* frame source */
        "camera_source", "camera_host", "camera_rtsp_port", "camera_channel",
        "camera_transport", "camera_user",
        /* detector */
        "cv_threads", "detect_roi", "detect_roi_align",
        /* vehicle counting */
        "detect_vehicles", "vehicle_conf_threshold", "vehicle_hold_frames",
        /* CLAHE contrast equalisation */
        "enhance_contrast", "clahe_clip_limit", "clahe_tile_grid",
        NULL
    };
    for (int i = 0; vision_keys[i]; i++)
        if (!strcmp(k, vision_keys[i]))
            return true;
    return false;
}

static void apply(const char *k, const char *v)
{
#define S(name, field) if (!strcmp(k, name)) { set_str(g_cfg.field, sizeof g_cfg.field, v); return; }
#define I(name, field) if (!strcmp(k, name)) { g_cfg.field = atoi(v); return; }
#define D(name, field) if (!strcmp(k, name)) { g_cfg.field = atof(v); return; }
#define B(name, field) if (!strcmp(k, name)) { g_cfg.field = truthy(v); return; }

    S("student_id",   student_id)
    S("student_name", student_name)

    I("http_port",     http_port)
    I("https_port",    https_port)
    I("loopback_port", loopback_port)
    S("public_host",   public_host)
    S("tls_cert",      tls_cert)
    S("tls_key",       tls_key)

    S("mqtt_host",               mqtt_host)
    S("mqtt_host_fallback",      mqtt_host_fallback)
    I("mqtt_port",               mqtt_port)
    S("mqtt_user",               mqtt_user)
    I("mqtt_keepalive",          mqtt_keepalive)
    I("mqtt_telemetry_interval", mqtt_telemetry_interval)

    S("smtp_url",                smtp_url)
    S("smtp_user",               smtp_user)
    S("mail_from",               mail_from)
    S("mail_to",                 mail_to)
    I("mail_debounce_sec",       mail_debounce_sec)
    I("mail_guard_debounce_sec", mail_guard_debounce_sec)

    B("telegram_enabled",            telegram_enabled)
    S("telegram_api_base",           telegram_api_base)
    S("telegram_chat_id",            telegram_chat_id)
    S("telegram_proxy_url",          telegram_proxy_url)
    S("telegram_proxy_user",         telegram_proxy_user)
    I("telegram_debounce_sec",       telegram_debounce_sec)
    I("telegram_guard_debounce_sec", telegram_guard_debounce_sec)
    B("telegram_commands_enabled",   telegram_commands_enabled)
    I("telegram_preview_cooldown_sec", telegram_preview_cooldown_sec)

    S("db_path",      db_path)
    I("db_ring_size", db_ring_size)

    I("target_fps",       target_fps)
    I("target_width",     target_width)
    I("target_net_input", target_net_input)

    I("watchdog_stale_sec", watchdog_stale_sec)
    D("thermal_high_c",     thermal_high_c)
    D("thermal_low_c",      thermal_low_c)
    B("guard_default",      guard_default)

#undef S
#undef I
#undef D
#undef B

    if (key_belongs_to_vision(k)) {
        LOGD("config: '%s' is the vision service's, not ours -- skipping", k);
        return;
    }
    LOGW("config: ignoring unknown key '%s' -- nothing in the system reads it", k);
}

/* A credential that appears in the config file is a configuration mistake we
 * refuse to tolerate silently: it would end up in the repository. */
static bool key_is_secretish(const char *k)
{
    static const char *banned[] = { "smtp_pass", "smtp_password", "mqtt_pass",
                                    "mqtt_password", "api_token", "password",
                                    "token", "secret", "telegram_token",
                                    "telegram_bot_token", "telegram_pass",
                                    "telegram_proxy_pass", NULL };
    for (int i = 0; banned[i]; i++)
        if (!strcasecmp(k, banned[i]))
            return true;
    return false;
}

static const char *require_env(const char *name)
{
    const char *v = getenv(name);
    if (!v || !*v) {
        LOGE("required secret %s is not set -- populate /etc/guardian/secrets.env "
             "(see config/secrets.env.example); refusing to start with a "
             "hard-coded fallback", name);
        return NULL;
    }
    return v;
}

bool config_load(const char *path)
{
    defaults();

    FILE *f = fopen(path, "r");
    if (!f) {
        LOGW("config: %s not readable, using built-in defaults", path);
    } else {
        char line[512];
        int lineno = 0;
        while (fgets(line, sizeof line, f)) {
            lineno++;
            char *s = trim(line);
            if (!*s || *s == '#' || *s == ';')
                continue;
            char *eq = strchr(s, '=');
            if (!eq) {
                LOGW("config:%d: no '=' -- skipping", lineno);
                continue;
            }
            *eq = '\0';
            char *k = trim(s);
            char *v = trim(eq + 1);
            strip_inline_comment(v);
            if (key_is_secretish(k)) {
                LOGE("config:%d: key '%s' must not live in %s -- secrets belong "
                     "in the systemd EnvironmentFile", lineno, k, path);
                fclose(f);
                return false;
            }
            apply(k, v);
        }
        fclose(f);
        LOGI("config: loaded %s", path);
    }

    /* Secrets: environment only. */
    g_cfg.smtp_pass = require_env("GUARDIAN_SMTP_PASS");
    g_cfg.mqtt_pass = require_env("GUARDIAN_MQTT_PASS");
    g_cfg.api_token = require_env("GUARDIAN_API_TOKEN");
    if (!g_cfg.smtp_pass || !g_cfg.mqtt_pass || !g_cfg.api_token)
        return false;

    if (strlen(g_cfg.api_token) < 16) {
        LOGE("GUARDIAN_API_TOKEN is shorter than 16 characters -- refusing");
        return false;
    }

    /* The Telegram channel is an addition to the mail one, not a replacement,
     * so a missing bot token disables it rather than stopping the daemon: the
     * board must keep guarding when the second channel is unconfigured. The
     * proxy password is optional even when the channel is on -- a deployment
     * that can reach api.telegram.org directly needs no proxy. */
    if (g_cfg.telegram_enabled) {
        g_cfg.telegram_token      = getenv("GUARDIAN_TELEGRAM_TOKEN");
        g_cfg.telegram_proxy_pass = getenv("GUARDIAN_TELEGRAM_PROXY_PASS");

        if (!g_cfg.telegram_token || !*g_cfg.telegram_token) {
            LOGW("telegram: GUARDIAN_TELEGRAM_TOKEN is not set -- the Telegram "
                 "channel is disabled (set it with mac/set_secret.sh)");
            g_cfg.telegram_enabled = false;
            g_cfg.telegram_token = NULL;
        } else if (!g_cfg.telegram_chat_id[0]) {
            LOGW("telegram: telegram_chat_id is empty in %s -- the Telegram "
                 "channel is disabled", path);
            g_cfg.telegram_enabled = false;
        }
        if (g_cfg.telegram_proxy_pass && !*g_cfg.telegram_proxy_pass)
            g_cfg.telegram_proxy_pass = NULL;
    }
    return true;
}

void config_log_summary(void)
{
    LOGI("identity   : %s (%s)", g_cfg.student_name, g_cfg.student_id);
    LOGI("web        : http:%d -> https:%d (loopback %d) host=%s",
         g_cfg.http_port, g_cfg.https_port, g_cfg.loopback_port, g_cfg.public_host);
    LOGI("mqtt       : %s@%s:%d (fallback %s) keepalive=%ds telemetry every %ds",
         g_cfg.mqtt_user, g_cfg.mqtt_host, g_cfg.mqtt_port,
         g_cfg.mqtt_host_fallback[0] ? g_cfg.mqtt_host_fallback : "none",
         g_cfg.mqtt_keepalive, g_cfg.mqtt_telemetry_interval);
    LOGI("mail       : %s -> %s via %s (debounce %ds, guard %ds)",
         g_cfg.mail_from, g_cfg.mail_to, g_cfg.smtp_url,
         g_cfg.mail_debounce_sec, g_cfg.mail_guard_debounce_sec);
    if (g_cfg.telegram_enabled) {
        LOGI("telegram   : chat %s via %s (proxy %s, debounce %ds, guard %ds)",
             g_cfg.telegram_chat_id, g_cfg.telegram_api_base,
             g_cfg.telegram_proxy_url[0] ? g_cfg.telegram_proxy_url : "none",
             g_cfg.telegram_debounce_sec, g_cfg.telegram_guard_debounce_sec);
        if (g_cfg.telegram_commands_enabled)
            LOGI("tg commands: enabled, read-only (/preview cooldown %ds)",
                 g_cfg.telegram_preview_cooldown_sec);
        else
            LOGI("tg commands: disabled (alerts still send)");
    } else {
        LOGI("telegram   : disabled");
    }
    LOGI("storage    : %s (ring %d)", g_cfg.db_path, g_cfg.db_ring_size);
    LOGI("vision     : capture %dpx @ %d fps, network input %dpx",
         g_cfg.target_width, g_cfg.target_fps, g_cfg.target_net_input);
    LOGI("watchdog   : stale after %ds", g_cfg.watchdog_stale_sec);
    LOGI("thermal    : throttle above %.1f C, recover below %.1f C",
         g_cfg.thermal_high_c, g_cfg.thermal_low_c);
    LOGI("secrets    : SMTP=<set,%zu chars> MQTT=<set,%zu chars> TOKEN=<set,%zu chars> "
         "TG=%s TGPROXY=%s",
         strlen(g_cfg.smtp_pass), strlen(g_cfg.mqtt_pass), strlen(g_cfg.api_token),
         g_cfg.telegram_token ? "<set>" : "<unset>",
         g_cfg.telegram_proxy_pass ? "<set>" : "<unset>");
}
