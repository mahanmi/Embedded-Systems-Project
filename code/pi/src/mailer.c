#include "mailer.h"

#include "common.h"
#include "config.h"
#include "log.h"
#include "shmframe.h"
#include "state.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ base64 */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* RFC 2045 requires attachment lines be wrapped; 76 chars is the norm. */
static char *base64_mime(const uint8_t *in, size_t len, size_t *out_len)
{
    size_t groups = (len + 2) / 3;
    size_t raw = groups * 4;
    size_t lines = (raw + 75) / 76;
    size_t cap = raw + lines * 2 + 4;

    char *out = malloc(cap);
    if (!out)
        return NULL;

    size_t o = 0, col = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)in[i + 2];

        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 0x3F] : '=';

        col += 4;
        if (col >= 76) {
            out[o++] = '\r';
            out[o++] = '\n';
            col = 0;
        }
    }
    if (col) {
        out[o++] = '\r';
        out[o++] = '\n';
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

/* --------------------------------------------------- outgoing message */

struct outgoing {
    char     subject[256];
    char     body[2048];
    uint8_t *jpeg;
    size_t   jpeg_len;
};

/* libcurl pulls the message body through this callback. */
struct upload_ctx {
    const char *data;
    size_t len;
    size_t sent;
};

static size_t read_cb(char *buf, size_t sz, size_t n, void *userp)
{
    struct upload_ctx *u = userp;
    size_t room = sz * n;
    size_t left = u->len - u->sent;
    size_t take = left < room ? left : room;
    if (take) {
        memcpy(buf, u->data + u->sent, take);
        u->sent += take;
    }
    return take;
}

/* Builds the full RFC 5322 message. Caller frees. */
static char *build_mime(const struct outgoing *m, size_t *out_len)
{
    char date_hdr[128];
    {
        time_t t = (time_t)now_wall();
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(date_hdr, sizeof date_hdr, "%a, %d %b %Y %H:%M:%S %z", &tm);
    }

    /* Deterministic-enough boundary; collision with JPEG base64 is impossible
     * because the alphabet excludes '='-padded runs of this shape. */
    char boundary[64];
    snprintf(boundary, sizeof boundary, "==guardian-%s-%ld==",
             g_cfg.student_id, (long)now_wall());

    char *b64 = NULL;
    size_t b64_len = 0;
    if (m->jpeg && m->jpeg_len) {
        b64 = base64_mime(m->jpeg, m->jpeg_len, &b64_len);
        if (!b64)
            LOGW("mailer: could not base64 the attachment, sending text only");
    }

    size_t cap = 4096 + strlen(m->body) + b64_len;
    char *msg = malloc(cap);
    if (!msg) {
        free(b64);
        return NULL;
    }

    size_t o = 0;
    o += (size_t)snprintf(msg + o, cap - o,
        "Date: %s\r\n"
        "To: %s\r\n"
        "From: Guardian %s <%s>\r\n"
        "Subject: %s\r\n"
        "MIME-Version: 1.0\r\n"
        "X-Guardian-Student-Id: %s\r\n"
        "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
        "\r\n"
        "--%s\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 8bit\r\n"
        "\r\n"
        "%s\r\n",
        date_hdr, g_cfg.mail_to, g_cfg.student_id, g_cfg.mail_from,
        m->subject, g_cfg.student_id, boundary, boundary, m->body);

    if (b64) {
        o += (size_t)snprintf(msg + o, cap - o,
            "\r\n--%s\r\n"
            "Content-Type: image/jpeg; name=\"detection.jpg\"\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "Content-Disposition: attachment; filename=\"detection.jpg\"\r\n"
            "\r\n", boundary);
        memcpy(msg + o, b64, b64_len);
        o += b64_len;
        free(b64);
    }

    o += (size_t)snprintf(msg + o, cap - o, "\r\n--%s--\r\n", boundary);

    if (out_len)
        *out_len = o;
    return msg;
}

static bool smtp_send(const struct outgoing *m)
{
    size_t msg_len = 0;
    char *msg = build_mime(m, &msg_len);
    if (!msg)
        return false;

    CURL *c = curl_easy_init();
    if (!c) {
        free(msg);
        return false;
    }

    struct curl_slist *rcpts = curl_slist_append(NULL, g_cfg.mail_to);
    struct upload_ctx up = { .data = msg, .len = msg_len, .sent = 0 };

    curl_easy_setopt(c, CURLOPT_URL, g_cfg.smtp_url);
    curl_easy_setopt(c, CURLOPT_USERNAME, g_cfg.smtp_user);
    curl_easy_setopt(c, CURLOPT_PASSWORD, g_cfg.smtp_pass); /* from the env */
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, g_cfg.mail_from);
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpts);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(c, CURLOPT_READDATA, &up);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        LOGE("mailer: send failed: %s", curl_easy_strerror(rc));

    curl_slist_free_all(rcpts);
    curl_easy_cleanup(c);
    free(msg);
    return rc == CURLE_OK;
}

/* ------------------------------------------------- queue + debounce */

#define MAILQ_CAP 8

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    bool            running;
    bool            healthy;

    /* Operational alerts: a small FIFO. */
    struct outgoing q[MAILQ_CAP];
    int             qhead, qcount;

    /* Detection alerts: exactly one pending slot (see the header comment). */
    bool            det_pending;
    struct outgoing det;
    double          det_due;        /* monotonic; when it may be sent      */
    double          last_det_send;  /* monotonic                           */

    /* Per-kind floors for operational alerts. */
    double last_kind_send[4];
} M = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cv   = PTHREAD_COND_INITIALIZER,
};

static void outgoing_free(struct outgoing *m)
{
    free(m->jpeg);
    m->jpeg = NULL;
    m->jpeg_len = 0;
}

static void *worker(void *arg)
{
    (void)arg;
    while (true) {
        struct outgoing job;
        bool have = false;

        pthread_mutex_lock(&M.lock);
        while (M.running && !have) {
            double now = now_mono();

            if (M.qcount > 0) {
                job = M.q[M.qhead];
                M.qhead = (M.qhead + 1) % MAILQ_CAP;
                M.qcount--;
                have = true;
                break;
            }
            if (M.det_pending && now >= M.det_due) {
                job = M.det;
                M.det_pending = false;
                M.det.jpeg = NULL; /* ownership moves to `job` */
                M.last_det_send = now;
                have = true;
                break;
            }

            if (M.det_pending) {
                /* Sleep exactly until the debounce window opens. */
                double wait = M.det_due - now;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec  += (time_t)wait;
                ts.tv_nsec += (long)((wait - (double)(time_t)wait) * 1e9);
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&M.cv, &M.lock, &ts);
            } else {
                pthread_cond_wait(&M.cv, &M.lock);
            }
        }
        bool running = M.running;
        pthread_mutex_unlock(&M.lock);

        if (!have) {
            if (!running)
                break;
            continue;
        }

        double t0 = now_mono();
        bool ok = smtp_send(&job);
        LOGI("mailer: \"%s\" %s in %.1fs%s", job.subject,
             ok ? "delivered" : "FAILED", now_mono() - t0,
             job.jpeg_len ? " (with attachment)" : "");
        if (ok) {
            state_inc_mail_sent();
            pthread_mutex_lock(&M.lock);
            M.healthy = true;
            pthread_mutex_unlock(&M.lock);
        }
        outgoing_free(&job);
    }
    return NULL;
}

bool mailer_start(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    M.running = true;
    M.qhead = M.qcount = 0;
    M.det_pending = false;
    /* Allow the very first detection to go out immediately. */
    M.last_det_send = now_mono() - 3600.0;
    for (int i = 0; i < 4; i++)
        M.last_kind_send[i] = M.last_det_send;

    if (pthread_create(&M.thread, NULL, worker, NULL) != 0) {
        LOGE("mailer: cannot start worker: %s", strerror(errno));
        M.running = false;
        return false;
    }
    LOGI("mailer: worker started (debounce %ds, guard %ds)",
         g_cfg.mail_debounce_sec, g_cfg.mail_guard_debounce_sec);
    return true;
}

void mailer_stop(void)
{
    pthread_mutex_lock(&M.lock);
    if (!M.running) {
        pthread_mutex_unlock(&M.lock);
        return;
    }
    M.running = false;
    pthread_cond_broadcast(&M.cv);
    pthread_mutex_unlock(&M.lock);
    pthread_join(M.thread, NULL);

    if (M.det_pending)
        outgoing_free(&M.det);
    while (M.qcount--) {
        outgoing_free(&M.q[M.qhead]);
        M.qhead = (M.qhead + 1) % MAILQ_CAP;
    }
    curl_global_cleanup();
}

bool mailer_notify_detection(int persons, double ts_wall, double cpu_temp_c,
                             const uint8_t *jpeg, size_t jpeg_len,
                             bool guard_mode)
{
    double debounce = guard_mode ? (double)g_cfg.mail_guard_debounce_sec
                                 : (double)g_cfg.mail_debounce_sec;

    char local[40], utc[40];
    iso8601_local(ts_wall, local, sizeof local);
    iso8601_utc(ts_wall, utc, sizeof utc);

    struct outgoing m;
    memset(&m, 0, sizeof m);
    snprintf(m.subject, sizeof m.subject,
             "%s[Guardian %s] %d person%s detected",
             guard_mode ? "*** ALARM *** " : "", g_cfg.student_id, persons,
             persons == 1 ? "" : "s");
    snprintf(m.body, sizeof m.body,
             "Smart Guardian System - detection alert\r\n"
             "=======================================\r\n"
             "\r\n"
             "  Student        : %s (%s)\r\n"
             "  Persons        : %d\r\n"
             "  Timestamp      : %s (local)\r\n"
             "                   %s\r\n"
             "  CPU temperature: %.1f C\r\n"
             "  Guard mode     : %s\r\n"
             "\r\n"
             "The frame captured at the moment of detection is attached.\r\n",
             g_cfg.student_name, g_cfg.student_id, persons, local, utc,
             cpu_temp_c, guard_mode ? "ARMED" : "off");

    if (jpeg && jpeg_len) {
        m.jpeg = malloc(jpeg_len);
        if (m.jpeg) {
            memcpy(m.jpeg, jpeg, jpeg_len);
            m.jpeg_len = jpeg_len;
        }
    }

    pthread_mutex_lock(&M.lock);
    double now = now_mono();
    double earliest = M.last_det_send + debounce;

    if (M.det_pending) {
        /* Coalesce: the newer observation replaces the queued one, and the
         * due time stays put so the interval guarantee is preserved. */
        outgoing_free(&M.det);
        state_inc_mail_suppressed();
    }
    M.det = m;
    M.det_pending = true;
    M.det_due = (now >= earliest) ? now : earliest;

    bool immediate = (M.det_due <= now);
    if (!immediate)
        LOGI("mailer: detection coalesced, will send in %.1fs (debounce %.0fs)",
             M.det_due - now, debounce);

    pthread_cond_broadcast(&M.cv);
    pthread_mutex_unlock(&M.lock);
    return immediate;
}

void mailer_notify_event(mail_kind_t kind, const char *subject,
                         const char *body, const uint8_t *jpeg, size_t jpeg_len)
{
    /* A flapping watchdog or an oscillating thermal governor must not turn
     * into an inbox flood; 120 s between messages of the same kind. */
    static const double KIND_FLOOR = 120.0;

    pthread_mutex_lock(&M.lock);
    double now = now_mono();
    if (now - M.last_kind_send[kind] < KIND_FLOOR) {
        state_inc_mail_suppressed();
        pthread_mutex_unlock(&M.lock);
        LOGI("mailer: '%s' suppressed (same kind sent %.0fs ago)", subject,
             now - M.last_kind_send[kind]);
        return;
    }
    if (M.qcount >= MAILQ_CAP) {
        pthread_mutex_unlock(&M.lock);
        LOGW("mailer: queue full, dropping '%s'", subject);
        return;
    }
    M.last_kind_send[kind] = now;

    struct outgoing *slot = &M.q[(M.qhead + M.qcount) % MAILQ_CAP];
    memset(slot, 0, sizeof *slot);
    snprintf(slot->subject, sizeof slot->subject, "%s", subject);
    snprintf(slot->body, sizeof slot->body, "%s", body);
    if (jpeg && jpeg_len) {
        slot->jpeg = malloc(jpeg_len);
        if (slot->jpeg) {
            memcpy(slot->jpeg, jpeg, jpeg_len);
            slot->jpeg_len = jpeg_len;
        }
    }
    M.qcount++;
    pthread_cond_broadcast(&M.cv);
    pthread_mutex_unlock(&M.lock);
}

bool mailer_healthy(void)
{
    pthread_mutex_lock(&M.lock);
    bool h = M.healthy;
    pthread_mutex_unlock(&M.lock);
    return h;
}
