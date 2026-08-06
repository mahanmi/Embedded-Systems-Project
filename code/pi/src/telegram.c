#include "telegram.h"

#include "common.h"
#include "config.h"
#include "log.h"
#include "shmframe.h"
#include "state.h"
#include "sysinfo.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bot API caption limit. sendMessage allows 4096, but one buffer for both
 * keeps the queue entries a fixed size and the callers already send short
 * summaries. */
#define TG_CAPTION_MAX 1024

/* Telegram error bodies are a couple of hundred bytes; a success body for
 * sendPhoto is larger but we never need to read it. */
#define TG_REPLY_MAX 4096

#define TGQ_CAP 8

/* One retry, three seconds later. The mailer deliberately does not retry --
 * against Gmail a failure is usually real -- but this channel goes through a
 * consumer SOCKS proxy where a single dropped connection is routine, and one
 * cheap second attempt converts most of those into delivered alerts. Two
 * attempts total, so a genuine outage still shows up as consecutive_failures
 * rather than a thread stuck retrying forever. */
#define TG_ATTEMPTS   2
#define TG_RETRY_WAIT 3.0

/* --------------------------------------------------- outgoing message */

struct tg_msg {
    char     caption[TG_CAPTION_MAX];
    uint8_t *jpeg;
    size_t   jpeg_len;
};

struct reply_buf {
    char   data[TG_REPLY_MAX];
    size_t len;
};

static size_t reply_cb(char *ptr, size_t sz, size_t n, void *userp)
{
    struct reply_buf *r = userp;
    size_t got = sz * n;
    size_t room = sizeof r->data - 1 - r->len;
    size_t take = got < room ? got : room;
    if (take) {
        memcpy(r->data + r->len, ptr, take);
        r->len += take;
        r->data[r->len] = '\0';
    }
    /* Claim the whole chunk regardless: returning short aborts the transfer,
     * and an oversized success body is not a reason to fail the send. */
    return got;
}

/* snprintf() truncates at a byte boundary, which can leave a half-written
 * UTF-8 sequence at the end of a caption -- and the Bot API rejects the whole
 * request when it cannot decode one. Walk back off any trailing continuation
 * bytes and the lead byte that started them. */
static void trim_partial_utf8(char *s)
{
    size_t n = strlen(s);
    if (!n)
        return;

    /* Walk back over trailing continuation bytes (10xxxxxx) to their lead. A
     * sequence is at most four bytes, so at most three of them. */
    size_t cont = 0;
    while (cont < n && cont < 3 && ((unsigned char)s[n - 1 - cont] & 0xC0) == 0x80)
        cont++;
    if (cont >= n)
        return;

    unsigned char lead = (unsigned char)s[n - 1 - cont];
    if ((lead & 0x80) == 0)
        return;                   /* plain ASCII: nothing was cut in half */

    size_t need = (lead & 0xE0) == 0xC0 ? 2 :
                  (lead & 0xF0) == 0xE0 ? 3 :
                  (lead & 0xF8) == 0xF0 ? 4 : 0;
    if (need == 0 || cont + 1 < need)
        s[n - 1 - cont] = '\0';   /* incomplete or malformed; drop it whole */
}

/* Pulls "description" out of an error reply so the journal says *why* rather
 * than just "FAILED". Bot API errors look like:
 *     {"ok":false,"error_code":400,"description":"chat not found"}    */
static void reply_description(const struct reply_buf *r, char *out, size_t outsz)
{
    snprintf(out, outsz, "no reply body");
    if (!r->len)
        return;

    struct json_object *root = json_tokener_parse(r->data);
    if (!root) {
        snprintf(out, outsz, "unparseable reply");
        return;
    }
    struct json_object *desc = NULL;
    if (json_object_object_get_ex(root, "description", &desc))
        snprintf(out, outsz, "%s", json_object_get_string(desc));
    else
        snprintf(out, outsz, "no description");
    json_object_put(root);
}

/* ------------------------------------------------------------- transport */

/* TLS, timeouts and proxy, shared by the send path and the command poller so
 * the two cannot drift apart. The poller passes a longer timeout because it
 * deliberately blocks server-side for most of it. */
static void tg_transport_opts(CURL *c, long timeout_sec)
{
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    /* The proxy carries TCP only -- TLS is still terminated at Telegram, so
     * certificate verification above stays on and the proxy operator sees
     * ciphertext. socks5h (rather than socks5) also hands the hostname to the
     * proxy to resolve, which matters when the local resolver is the thing
     * being filtered. */
    if (g_cfg.telegram_proxy_url[0]) {
        curl_easy_setopt(c, CURLOPT_PROXY, g_cfg.telegram_proxy_url);
        curl_easy_setopt(c, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
        if (g_cfg.telegram_proxy_user[0] && g_cfg.telegram_proxy_pass) {
            char pw[192];
            snprintf(pw, sizeof pw, "%s:%s", g_cfg.telegram_proxy_user,
                     g_cfg.telegram_proxy_pass);
            /* libcurl copies the string, so the stack buffer is fine -- and it
             * is scrubbed rather than left on the stack for a later frame. */
            curl_easy_setopt(c, CURLOPT_PROXYUSERPWD, pw);
            memset(pw, 0, sizeof pw);
        }
    }
}

static bool tg_post(const struct tg_msg *m)
{
    CURL *c = curl_easy_init();
    if (!c)
        return false;

    /* The token sits in the request path, so this URL *is* a credential: it is
     * built here, used, and freed with the handle. It must never reach LOGx(),
     * CURLOPT_VERBOSE or an error string. */
    char url[320];
    snprintf(url, sizeof url, "%s/bot%s/%s", g_cfg.telegram_api_base,
             g_cfg.telegram_token, m->jpeg_len ? "sendPhoto" : "sendMessage");

    curl_mime *mime = curl_mime_init(c);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, g_cfg.telegram_chat_id, CURL_ZERO_TERMINATED);

    if (m->jpeg_len) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, m->caption, CURL_ZERO_TERMINATED);

        part = curl_mime_addpart(mime);
        curl_mime_name(part, "photo");
        curl_mime_filename(part, "detection.jpg");
        curl_mime_type(part, "image/jpeg");
        curl_mime_data(part, (const char *)m->jpeg, m->jpeg_len);
    } else {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "text");
        curl_mime_data(part, m->caption, CURL_ZERO_TERMINATED);
    }

    struct reply_buf reply = { .len = 0 };
    reply.data[0] = '\0';

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, reply_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &reply);
    tg_transport_opts(c, 45L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);

    /* Two failure modes, and only checking the first is a trap: a wrong chat
     * id or a bot the user has blocked completes the transfer perfectly well
     * and answers 4xx with {"ok":false}. Treating CURLE_OK as delivery would
     * report those as successes forever. */
    bool ok = (rc == CURLE_OK && http == 200);
    if (!ok) {
        if (rc != CURLE_OK) {
            LOGW("telegram: transport failed: %s", curl_easy_strerror(rc));
        } else {
            char why[192];
            reply_description(&reply, why, sizeof why);
            LOGW("telegram: API rejected the message (HTTP %ld): %s", http, why);
        }
    }

    memset(url, 0, sizeof url);
    curl_mime_free(mime);
    curl_easy_cleanup(c);
    return ok;
}

/* ------------------------------------------------- queue + debounce */

static struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    bool            running;
    bool            enabled;

    bool            attempted;
    bool            last_ok;
    double          last_attempt_wall;
    unsigned        consec_failures;

    struct tg_msg   q[TGQ_CAP];
    int             qhead, qcount;

    bool            det_pending;
    struct tg_msg   det;
    double          det_due;
    double          last_det_send;

    double last_kind_send[4];

    /* Inbound half. `poll_run` is separate from `running` so the poller can be
     * disabled (telegram_commands_enabled = false) while the send worker runs. */
    pthread_t poll_thread;
    bool      poll_run;
    bool      polling;
    unsigned  poll_failures;
    double    last_update_wall;
    uint64_t  commands_handled;
    double    last_preview;         /* monotonic; the /preview cooldown     */
    double    last_unauth_reply;    /* monotonic; anti-abuse floor          */
} T = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cv   = PTHREAD_COND_INITIALIZER,
};

static void tg_msg_free(struct tg_msg *m)
{
    free(m->jpeg);
    m->jpeg = NULL;
    m->jpeg_len = 0;
}

/* Sleep between retries without making shutdown wait for the full interval. */
static void retry_nap(double seconds)
{
    const double slice = 0.25;
    for (double t = 0; t < seconds; t += slice) {
        pthread_mutex_lock(&T.lock);
        bool running = T.running;
        pthread_mutex_unlock(&T.lock);
        if (!running)
            return;
        usleep((useconds_t)(slice * 1e6));
    }
}

/* First line of the caption, for the journal -- the rest is detail nobody
 * reads in a log. */
static void caption_headline(const char *caption, char *out, size_t outsz)
{
    size_t i = 0;
    while (caption[i] && caption[i] != '\n' && i + 1 < outsz)
        i++;
    snprintf(out, outsz, "%.*s", (int)i, caption);
}

static void *worker(void *arg)
{
    (void)arg;
    while (true) {
        struct tg_msg job;
        bool have = false;

        pthread_mutex_lock(&T.lock);
        while (T.running && !have) {
            double now = now_mono();

            if (T.qcount > 0) {
                job = T.q[T.qhead];
                T.qhead = (T.qhead + 1) % TGQ_CAP;
                T.qcount--;
                have = true;
                break;
            }
            if (T.det_pending && now >= T.det_due) {
                job = T.det;
                T.det_pending = false;
                T.det.jpeg = NULL;  /* ownership moves to `job` */
                T.last_det_send = now;
                have = true;
                break;
            }

            if (T.det_pending) {
                double wait = T.det_due - now;
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec  += (time_t)wait;
                ts.tv_nsec += (long)((wait - (double)(time_t)wait) * 1e9);
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                pthread_cond_timedwait(&T.cv, &T.lock, &ts);
            } else {
                pthread_cond_wait(&T.cv, &T.lock);
            }
        }
        bool running = T.running;
        pthread_mutex_unlock(&T.lock);

        if (!have) {
            if (!running)
                break;
            continue;
        }

        double t0 = now_mono();
        bool ok = false;
        int tries = 0;
        for (; tries < TG_ATTEMPTS && !ok; tries++) {
            if (tries > 0) {
                retry_nap(TG_RETRY_WAIT);
                pthread_mutex_lock(&T.lock);
                running = T.running;
                pthread_mutex_unlock(&T.lock);
                if (!running)
                    break;
            }
            ok = tg_post(&job);
        }

        char headline[80];
        caption_headline(job.caption, headline, sizeof headline);
        LOGI("telegram: \"%s\" %s in %.1fs%s%s", headline,
             ok ? "delivered" : "FAILED", now_mono() - t0,
             job.jpeg_len ? " (with photo)" : "",
             tries > 1 ? " [retried]" : "");
        if (ok)
            state_inc_telegram_sent();

        pthread_mutex_lock(&T.lock);
        T.attempted = true;
        T.last_ok = ok;
        T.last_attempt_wall = now_wall();
        T.consec_failures = ok ? 0u : T.consec_failures + 1u;
        unsigned fails = T.consec_failures;
        pthread_mutex_unlock(&T.lock);

        if (!ok && fails == TELEGRAM_FAIL_DEGRADE)
            LOGE("telegram: %u consecutive failures -- check the proxy is up "
                 "and GUARDIAN_TELEGRAM_TOKEN is current", fails);

        tg_msg_free(&job);
    }
    return NULL;
}

/* Defined with the rest of the inbound half at the bottom of this file. */
static void tg_poll_start(void);
static void tg_poll_stop(void);

bool telegram_start(void)
{
    /* An unconfigured channel is not a failure: mail still works and the board
     * must still guard. config_load() has already decided this and logged why. */
    if (!g_cfg.telegram_enabled) {
        T.enabled = false;
        LOGI("telegram: channel disabled, not starting a worker");
        return true;
    }

    T.enabled = true;
    T.running = true;
    T.qhead = T.qcount = 0;
    T.det_pending = false;
    T.attempted = false;
    T.last_ok = false;
    T.last_attempt_wall = 0.0;
    T.consec_failures = 0;
    T.last_det_send = now_mono() - 3600.0;   /* first detection goes at once */
    for (int i = 0; i < 4; i++)
        T.last_kind_send[i] = T.last_det_send;

    /* Half-configured proxy authentication fails at the first send, minutes
     * later and only in the journal. Say it now, while the reason is obvious. */
    if (g_cfg.telegram_proxy_url[0] && g_cfg.telegram_proxy_user[0] &&
        !g_cfg.telegram_proxy_pass)
        LOGW("telegram: telegram_proxy_user is set but "
             "GUARDIAN_TELEGRAM_PROXY_PASS is not -- the proxy will almost "
             "certainly refuse the connection");

    if (pthread_create(&T.thread, NULL, worker, NULL) != 0) {
        LOGE("telegram: cannot start worker: %s", strerror(errno));
        T.running = false;
        T.enabled = false;
        return false;
    }
    LOGI("telegram: worker started (chat %s, debounce %ds, guard %ds, %s)",
         g_cfg.telegram_chat_id, g_cfg.telegram_debounce_sec,
         g_cfg.telegram_guard_debounce_sec,
         g_cfg.telegram_proxy_url[0] ? "via SOCKS5 proxy" : "direct");

    /* Inbound is optional on top of outbound: a poller that cannot start is
     * not a reason to lose the alerts. */
    tg_poll_start();
    return true;
}

void telegram_stop(void)
{
    /* The poller first, and before the queue drain below: it enqueues replies,
     * so draining while it still runs could leave a message allocated after
     * the free. Stopping it here also guarantees it is gone before main()
     * reaches shmframe_close() and curl_global_cleanup(). */
    tg_poll_stop();

    pthread_mutex_lock(&T.lock);
    if (!T.running) {
        pthread_mutex_unlock(&T.lock);
        return;
    }
    T.running = false;
    pthread_cond_broadcast(&T.cv);
    pthread_mutex_unlock(&T.lock);
    pthread_join(T.thread, NULL);

    if (T.det_pending)
        tg_msg_free(&T.det);
    while (T.qcount--) {
        tg_msg_free(&T.q[T.qhead]);
        T.qhead = (T.qhead + 1) % TGQ_CAP;
    }
}

/* Copies the frame into the queue entry. The caller owns its guardian_frame
 * and will reuse it for the next read, so a pointer would dangle. */
static void attach_jpeg(struct tg_msg *m, const uint8_t *jpeg, size_t jpeg_len)
{
    if (!jpeg || !jpeg_len)
        return;
    m->jpeg = malloc(jpeg_len);
    if (m->jpeg) {
        memcpy(m->jpeg, jpeg, jpeg_len);
        m->jpeg_len = jpeg_len;
    }
}

bool telegram_notify_detection(int persons, double ts_wall, double cpu_temp_c,
                               const uint8_t *jpeg, size_t jpeg_len,
                               bool guard_mode)
{
    if (!T.enabled)
        return false;

    double debounce = guard_mode ? (double)g_cfg.telegram_guard_debounce_sec
                                 : (double)g_cfg.telegram_debounce_sec;

    char local[40];
    iso8601_local(ts_wall, local, sizeof local);

    struct tg_msg m;
    memset(&m, 0, sizeof m);
    snprintf(m.caption, sizeof m.caption,
             "%s%d person%s detected\n"
             "Time: %s\n"
             "CPU: %.1f C\n"
             "Guard: %s\n"
             "Guardian %s",
             guard_mode ? "*** ALARM *** " : "",
             persons, persons == 1 ? "" : "s", local, cpu_temp_c,
             guard_mode ? "ARMED" : "off", g_cfg.student_id);
    trim_partial_utf8(m.caption);
    attach_jpeg(&m, jpeg, jpeg_len);

    pthread_mutex_lock(&T.lock);
    double now = now_mono();
    double earliest = T.last_det_send + debounce;

    if (T.det_pending) {
        tg_msg_free(&T.det);
        state_inc_telegram_suppressed();
    }
    T.det = m;
    T.det_pending = true;
    T.det_due = (now >= earliest) ? now : earliest;

    bool immediate = (T.det_due <= now);
    if (!immediate)
        LOGI("telegram: detection coalesced, will send in %.1fs (debounce %.0fs)",
             T.det_due - now, debounce);

    pthread_cond_broadcast(&T.cv);
    pthread_mutex_unlock(&T.lock);
    return immediate;
}

void telegram_notify_event(mail_kind_t kind, const char *caption,
                           const uint8_t *jpeg, size_t jpeg_len)
{
    if (!T.enabled)
        return;

    /* Same 120 s per-kind floor as the mailer: a flapping watchdog must not
     * turn into a stream of phone notifications either. */
    static const double KIND_FLOOR = 120.0;

    char headline[80];
    caption_headline(caption, headline, sizeof headline);

    pthread_mutex_lock(&T.lock);
    double now = now_mono();
    if (now - T.last_kind_send[kind] < KIND_FLOOR) {
        double ago = now - T.last_kind_send[kind];
        state_inc_telegram_suppressed();
        pthread_mutex_unlock(&T.lock);
        LOGI("telegram: '%s' suppressed (same kind sent %.0fs ago)", headline, ago);
        return;
    }
    if (T.qcount >= TGQ_CAP) {
        pthread_mutex_unlock(&T.lock);
        LOGW("telegram: queue full, dropping '%s'", headline);
        return;
    }
    T.last_kind_send[kind] = now;

    struct tg_msg *slot = &T.q[(T.qhead + T.qcount) % TGQ_CAP];
    memset(slot, 0, sizeof *slot);
    snprintf(slot->caption, sizeof slot->caption, "%s", caption);
    trim_partial_utf8(slot->caption);
    attach_jpeg(slot, jpeg, jpeg_len);
    T.qcount++;

    pthread_cond_broadcast(&T.cv);
    pthread_mutex_unlock(&T.lock);
}

/* A reply to an inbound command. Deliberately NOT telegram_notify_event():
 *
 *   * no per-kind floor. That floor exists so a flapping watchdog cannot flood
 *     the phone, which is the right answer for an alert nobody asked for and
 *     the wrong one for an answer to a question somebody just asked. /preview
 *     has its own, much shorter cooldown instead.
 *
 *   * two queue slots are held back for alerts. Command replies are the only
 *     traffic a person can generate on demand, and an intrusion alarm losing
 *     its slot to a burst of /preview would invert the whole point of the
 *     system. Alerts always win.
 */
static bool tg_enqueue_reply(const char *caption, const uint8_t *jpeg,
                             size_t jpeg_len)
{
    const int RESERVED_FOR_ALERTS = 2;

    pthread_mutex_lock(&T.lock);
    if (T.qcount >= TGQ_CAP - RESERVED_FOR_ALERTS) {
        pthread_mutex_unlock(&T.lock);
        LOGW("telegram: queue busy, dropping a command reply "
             "(the remaining slots are reserved for alerts)");
        return false;
    }

    struct tg_msg *slot = &T.q[(T.qhead + T.qcount) % TGQ_CAP];
    memset(slot, 0, sizeof *slot);
    snprintf(slot->caption, sizeof slot->caption, "%s", caption);
    trim_partial_utf8(slot->caption);
    attach_jpeg(slot, jpeg, jpeg_len);
    T.qcount++;

    pthread_cond_broadcast(&T.cv);
    pthread_mutex_unlock(&T.lock);
    return true;
}

void telegram_health(struct telegram_health *out)
{
    pthread_mutex_lock(&T.lock);
    out->enabled              = T.enabled;
    out->attempted            = T.attempted;
    out->ok                   = !T.attempted || T.last_ok;
    out->last_attempt_wall    = T.last_attempt_wall;
    out->consecutive_failures = T.consec_failures;
    out->polling              = T.polling;
    out->poll_failures        = T.poll_failures;
    out->last_update_wall     = T.last_update_wall;
    out->commands_handled     = T.commands_handled;
    pthread_mutex_unlock(&T.lock);
}

bool telegram_healthy(void)
{
    struct telegram_health h;
    telegram_health(&h);
    return h.ok;
}

/* =========================================================================
 *  INBOUND: the command poller
 *
 *  getUpdates long polling, because a webhook is not available to this board:
 *  Telegram would have to open a connection inwards, and the only route this
 *  network has to the API is an outbound SOCKS5 proxy.
 * ========================================================================= */

/* Server-side block. Long enough that the board is idle almost all the time,
 * short enough that one lost connection is not a long silence. */
#define TG_POLL_TIMEOUT 25L

/* getUpdates answers are small -- one text message each -- but a burst of them
 * after a reconnect is not, so this is generous. */
#define TG_UPDATE_MAX 65536

/* An unknown sender gets told once, then not again for this long. Answering
 * every stranger's message would let anyone make this board push traffic
 * through the proxy simply by typing. */
#define TG_UNAUTH_FLOOR 300.0

/* Poll failures repeat every TG_POLL_TIMEOUT seconds. Through a night of proxy
 * downtime that is thousands of journal lines saying the same thing, so only
 * the first and then every Nth are logged, plus a line when it recovers. */
#define TG_FAIL_LOG_EVERY 50u

struct update_buf {
    char  *data;
    size_t len;
    size_t cap;
};

static size_t update_cb(char *ptr, size_t sz, size_t n, void *userp)
{
    struct update_buf *b = userp;
    size_t got = sz * n;
    if (b->len + got + 1 > b->cap)
        return 0;               /* over the cap: fail the transfer honestly */
    memcpy(b->data + b->len, ptr, got);
    b->len += got;
    b->data[b->len] = '\0';
    return got;
}

/* libcurl calls this about once a second even while the long poll is idle,
 * which is what keeps shutdown prompt: without it, stopping the daemon would
 * wait out the full TG_POLL_TIMEOUT. */
static int poll_progress_cb(void *clientp, curl_off_t dt, curl_off_t dn,
                            curl_off_t ut, curl_off_t un)
{
    (void)clientp; (void)dt; (void)dn; (void)ut; (void)un;
    bool run;
    pthread_mutex_lock(&T.lock);
    run = T.poll_run;
    pthread_mutex_unlock(&T.lock);
    return run ? 0 : 1;         /* non-zero aborts the transfer */
}

static bool poll_running(void)
{
    bool run;
    pthread_mutex_lock(&T.lock);
    run = T.poll_run;
    pthread_mutex_unlock(&T.lock);
    return run;
}

static void poll_nap(double seconds)
{
    const double slice = 0.25;
    for (double t = 0; t < seconds; t += slice) {
        if (!poll_running())
            return;
        usleep((useconds_t)(slice * 1e6));
    }
}

/* Performs one getUpdates call. Returns the parsed root on success (caller
 * json_object_put()s it), NULL otherwise. */
static struct json_object *tg_get_updates(CURL *c, struct update_buf *buf,
                                          long long offset, long timeout,
                                          int limit)
{
    /* Same discipline as tg_post(): the token is in the path, so this string is
     * a credential and must never reach a log line or an error message. */
    char url[512];
    snprintf(url, sizeof url,
             "%s/bot%s/getUpdates?offset=%lld&timeout=%ld&limit=%d"
             "&allowed_updates=%%5B%%22message%%22%%5D",
             g_cfg.telegram_api_base, g_cfg.telegram_token, offset, timeout,
             limit);

    buf->len = 0;
    buf->data[0] = '\0';

    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, update_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, poll_progress_cb);
    /* Must outlast the server-side block, or every poll would "time out". */
    tg_transport_opts(c, timeout + 15L);

    CURLcode rc = curl_easy_perform(c);
    long http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
    memset(url, 0, sizeof url);

    if (rc != CURLE_OK) {
        /* Our own abort during shutdown is not a failure worth reporting. */
        if (rc == CURLE_ABORTED_BY_CALLBACK && !poll_running())
            return NULL;
        pthread_mutex_lock(&T.lock);
        unsigned n = ++T.poll_failures;
        pthread_mutex_unlock(&T.lock);
        if (n == 1 || n % TG_FAIL_LOG_EVERY == 0)
            LOGW("telegram: getUpdates failed (%u in a row): %s", n,
                 curl_easy_strerror(rc));
        return NULL;
    }

    if (http != 200) {
        struct reply_buf rb;
        rb.len = buf->len < sizeof rb.data - 1 ? buf->len : sizeof rb.data - 1;
        memcpy(rb.data, buf->data, rb.len);
        rb.data[rb.len] = '\0';
        char why[192];
        reply_description(&rb, why, sizeof why);

        pthread_mutex_lock(&T.lock);
        unsigned n = ++T.poll_failures;
        pthread_mutex_unlock(&T.lock);

        /* Two answers deserve their own words: a generic "API rejected it"
         * would send you looking in entirely the wrong place. */
        if (http == 409)
            LOGE("telegram: getUpdates conflict (HTTP 409): another process is "
                 "polling this bot, or a webhook is set -- call deleteWebhook");
        else if (http == 401)
            LOGE("telegram: getUpdates unauthorised (HTTP 401): the bot token "
                 "has been revoked; set a new one with set_secret.sh");
        else if (n == 1 || n % TG_FAIL_LOG_EVERY == 0)
            LOGW("telegram: getUpdates rejected (HTTP %ld, %u in a row): %s",
                 http, n, why);
        return NULL;
    }

    struct json_object *root = json_tokener_parse(buf->data);
    if (!root) {
        LOGW("telegram: getUpdates returned unparseable JSON (%zu bytes)",
             buf->len);
        return NULL;
    }
    return root;
}

/* ------------------------------------------------------ command handlers */

static void cmd_preview(void)
{
    /* Cooldown first: no point reading a 257 KB frame to then discard it. */
    pthread_mutex_lock(&T.lock);
    double now = now_mono();
    double since = now - T.last_preview;
    double cool = (double)g_cfg.telegram_preview_cooldown_sec;
    bool too_soon = (T.last_preview > 0.0 && since < cool);
    if (!too_soon)
        T.last_preview = now;
    pthread_mutex_unlock(&T.lock);

    if (too_soon) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "Slow down -- try again in %.0f s.", cool - since);
        tg_enqueue_reply(msg, NULL, 0);
        return;
    }

    /* 257 KB: far too big for a thread stack, same reason as cmd_snapshot(). */
    struct guardian_frame *f = malloc(sizeof *f);
    if (!f) {
        tg_enqueue_reply("The board is out of memory.", NULL, 0);
        return;
    }

    if (!shmframe_read(f, true) || !f->jpeg_len) {
        free(f);
        tg_enqueue_reply("No frame yet -- the detector has not published one. "
                         "It may still be loading its model.", NULL, 0);
        return;
    }

    /* A synthetic frame is the detector's "waiting for the capture host"
     * placeholder. Sending it would be a photo of a grey box; saying so, with
     * how long the signal has been gone, is the useful answer. */
    if (f->flags & GUARDIAN_FRAME_SYNTHETIC) {
        double real_mono = 0.0;
        char msg[256];
        if (shmframe_last_real_ts(&real_mono))
            snprintf(msg, sizeof msg,
                     "No camera signal.\n"
                     "Last real frame was %.0f s ago.\n"
                     "The detector is alive and publishing placeholders.",
                     now_mono() - real_mono);
        else
            snprintf(msg, sizeof msg,
                     "No camera signal.\n"
                     "The capture host has never connected since this daemon "
                     "started.");
        free(f);
        tg_enqueue_reply(msg, NULL, 0);
        return;
    }

    char iso[40];
    iso8601_local(f->ts_wall, iso, sizeof iso);

    char cap[TG_CAPTION_MAX];
    int n = snprintf(cap, sizeof cap,
                     "Live preview\n"
                     "Persons: %d\n",
                     f->persons);
    if (f->vehicles > 0)
        n += snprintf(cap + n, sizeof cap - (size_t)n,
                      "Vehicles: %d\n", f->vehicles);
    snprintf(cap + n, sizeof cap - (size_t)n,
             "FPS: %.1f\n"
             "CPU: %.1f C\n"
             "Guard: %s\n"
             "Time: %s\n"
             "Video: https://%s/api/v1/stream",
             (double)f->fps, sysinfo_cpu_temp(),
             state_guard_mode() ? "ARMED" : "off", iso, g_cfg.public_host);
    trim_partial_utf8(cap);

    tg_enqueue_reply(cap, f->jpeg, f->jpeg_len);
    free(f);
}

static void cmd_help(void);

static void cmd_start(void)
{
    char msg[512];
    snprintf(msg, sizeof msg,
             "Smart Guardian System -- Guardian %s\n"
             "\n"
             "This chat is authorised. You will get a photo here whenever the "
             "board detects someone, and alerts if the camera is tampered with "
             "or the board overheats.\n"
             "\n"
             "Send /help to see what you can ask it.",
             g_cfg.student_id);
    tg_enqueue_reply(msg, NULL, 0);
}

/* The command table. Read-only by construction: arming the alarm, changing the
 * frame rate and rebooting stay behind the REST API's bearer token, because a
 * chat is authenticated by whoever is holding the phone. */
static const struct tg_cmd {
    const char *name;
    const char *summary;
    void      (*fn)(void);
} TG_COMMANDS[] = {
    { "/preview", "a photo of what the camera sees right now", cmd_preview },
    { "/help",    "this list",                                 cmd_help    },
    { "/start",   "what this bot is",                          cmd_start   },
};
#define TG_NCOMMANDS ((int)(sizeof TG_COMMANDS / sizeof TG_COMMANDS[0]))

static void cmd_help(void)
{
    char msg[512];
    int n = snprintf(msg, sizeof msg, "Commands:\n");
    for (int i = 0; i < TG_NCOMMANDS; i++)
        n += snprintf(msg + n, sizeof msg - (size_t)n, "%s  -- %s\n",
                      TG_COMMANDS[i].name, TG_COMMANDS[i].summary);
    snprintf(msg + n, sizeof msg - (size_t)n,
             "\nAll of them are read-only. Arming the alarm and changing "
             "settings need the REST API and its token.");
    tg_enqueue_reply(msg, NULL, 0);
}

/* Normalises "/Preview@MahanSmartGaurdianBot  extra words" to "/preview".
 * Telegram appends @BotName whenever the bot is in a group, and it is easy to
 * send a capitalised command from a phone keyboard. */
static void normalise_command(const char *text, char *out, size_t outsz)
{
    size_t i = 0;
    while (text[i] && !isspace((unsigned char)text[i]) && text[i] != '@' &&
           i + 1 < outsz) {
        out[i] = (char)tolower((unsigned char)text[i]);
        i++;
    }
    out[i] = '\0';
}

/* Handles one message object. Everything upstream of this is transport. */
static void handle_message(struct json_object *msg)
{
    struct json_object *chat = NULL, *jtext = NULL, *jid = NULL;

    if (!json_object_object_get_ex(msg, "chat", &chat) ||
        !json_object_object_get_ex(chat, "id", &jid))
        return;                                 /* not a chat message */

    /* Chat ids are 64-bit and can be negative for groups; compare as text so
     * this matches whatever the operator wrote in guardian.conf. */
    char from[64];
    snprintf(from, sizeof from, "%lld", (long long)json_object_get_int64(jid));

    /* ---- authorisation: the load-bearing check ----
     * Anyone can message a bot. Nothing below this point runs, and no frame is
     * read, unless the sender is the configured chat. */
    if (strcmp(from, g_cfg.telegram_chat_id) != 0) {
        state_inc_command(false);

        pthread_mutex_lock(&T.lock);
        double now = now_mono();
        bool may_reply = (T.last_unauth_reply == 0.0 ||
                          now - T.last_unauth_reply >= TG_UNAUTH_FLOOR);
        if (may_reply)
            T.last_unauth_reply = now;
        pthread_mutex_unlock(&T.lock);

        LOGW("telegram: refused a command from chat %s (not the configured "
             "chat)%s", from, may_reply ? "" : " [reply suppressed]");

        /* Telling a stranger is the operator's choice; letting a stranger make
         * the board send on demand is not, so this is floored hard. */
        if (may_reply)
            tg_enqueue_reply("Not authorised.", NULL, 0);
        return;
    }

    if (!json_object_object_get_ex(msg, "text", &jtext))
        return;                                 /* a sticker, a photo, ... */
    const char *text = json_object_get_string(jtext);
    if (!text || *text != '/')
        return;                                 /* ordinary chatter */

    char cmd[64];
    normalise_command(text, cmd, sizeof cmd);

    for (int i = 0; i < TG_NCOMMANDS; i++) {
        if (strcmp(cmd, TG_COMMANDS[i].name) != 0)
            continue;

        LOGI("telegram: command %s", cmd);
        pthread_mutex_lock(&T.lock);
        T.last_update_wall = now_wall();
        T.commands_handled++;
        pthread_mutex_unlock(&T.lock);

        state_inc_command(true);
        TG_COMMANDS[i].fn();
        return;
    }

    state_inc_command(false);
    LOGI("telegram: unknown command '%s'", cmd);
    {
        char msg2[128];
        snprintf(msg2, sizeof msg2, "I do not know %s. Try /help.", cmd);
        tg_enqueue_reply(msg2, NULL, 0);
    }
}

/* ------------------------------------------------------------- the loop */

/* Walks the result array and returns the offset to ask for next. The offset
 * advances only past updates we have finished with, so a crash mid-batch
 * replays them rather than losing them.
 *
 * `dispatch` is false for the startup priming call, which exists purely to
 * learn where the backlog ends. Getting that wrong is not a cosmetic bug: the
 * priming call really does hand back a stale command, and acting on it means
 * answering a message that may be hours old as though it had just arrived. */
static long long process_updates(struct json_object *root, long long offset,
                                 bool dispatch)
{
    struct json_object *ok = NULL, *result = NULL;

    if (json_object_object_get_ex(root, "ok", &ok) &&
        !json_object_get_boolean(ok))
        return offset;
    if (!json_object_object_get_ex(root, "result", &result) ||
        !json_object_is_type(result, json_type_array))
        return offset;

    size_t n = json_object_array_length(result);
    for (size_t i = 0; i < n; i++) {
        struct json_object *upd = json_object_array_get_idx(result, i);
        struct json_object *jid = NULL, *msg = NULL;

        if (!upd || !json_object_object_get_ex(upd, "update_id", &jid))
            continue;
        long long id = (long long)json_object_get_int64(jid);

        if (dispatch && json_object_object_get_ex(upd, "message", &msg))
            handle_message(msg);

        if (id + 1 > offset)
            offset = id + 1;
    }
    return offset;
}

static void *poll_worker(void *arg)
{
    (void)arg;

    struct update_buf buf = { .data = malloc(TG_UPDATE_MAX), .len = 0,
                              .cap = TG_UPDATE_MAX };
    CURL *c = curl_easy_init();
    if (!buf.data || !c) {
        LOGE("telegram: cannot allocate the command poller");
        free(buf.data);
        if (c) curl_easy_cleanup(c);
        pthread_mutex_lock(&T.lock);
        T.polling = false;
        pthread_mutex_unlock(&T.lock);
        return NULL;
    }

    /* Prime the offset. The bot has been send-only until now, so Telegram is
     * holding every message ever sent to it (up to 24 h). Without this, the
     * first poll would replay all of them as fresh commands. Ask for just the
     * newest and start after it. */
    long long offset = 0;
    struct json_object *root = tg_get_updates(c, &buf, -1, 0, 1);
    if (root) {
        long long newest = process_updates(root, 0, false);   /* learn, do not act */
        json_object_put(root);
        if (newest > 0) {
            offset = newest;
            LOGI("telegram: skipping the backlog, polling from update %lld",
                 offset);
        }
    }
    /* A failed priming call leaves offset at 0, which means "give me the whole
     * backlog" -- exactly what priming exists to avoid. Rather than replay it,
     * keep polling with 0 but treat the first batch as stale too. */
    bool primed = (offset > 0);

    pthread_mutex_lock(&T.lock);
    T.poll_failures = 0;
    pthread_mutex_unlock(&T.lock);

    LOGI("telegram: command poller listening (/preview, /help, /start)");

    while (poll_running()) {
        root = tg_get_updates(c, &buf, offset, TG_POLL_TIMEOUT, 20);
        if (!root) {
            if (!poll_running())
                break;
            /* Back off a little so a hard failure (DNS, proxy down) does not
             * become a tight loop; a long poll that merely returned empty is
             * not an error and does not reach here. */
            poll_nap(5.0);
            continue;
        }

        pthread_mutex_lock(&T.lock);
        unsigned was = T.poll_failures;
        T.poll_failures = 0;
        pthread_mutex_unlock(&T.lock);
        if (was >= TG_FAIL_LOG_EVERY)
            LOGI("telegram: getUpdates recovered after %u failures", was);

        /* If priming failed, the first batch back is whatever Telegram had
         * queued -- consume it for its offset only, then behave normally. */
        offset = process_updates(root, offset, primed);
        if (!primed) {
            primed = true;
            LOGI("telegram: discarded the backlog, polling from update %lld",
                 offset);
        }
        json_object_put(root);
    }

    curl_easy_cleanup(c);
    free(buf.data);
    pthread_mutex_lock(&T.lock);
    T.polling = false;
    pthread_mutex_unlock(&T.lock);
    LOGI("telegram: command poller stopped");
    return NULL;
}

static void tg_poll_start(void)
{
    if (!g_cfg.telegram_commands_enabled) {
        LOGI("telegram: commands disabled, not polling for updates");
        return;
    }

    pthread_mutex_lock(&T.lock);
    T.poll_run = true;
    T.polling  = true;
    T.poll_failures = 0;
    T.last_preview = 0.0;
    T.last_unauth_reply = 0.0;
    T.commands_handled = 0;
    T.last_update_wall = 0.0;
    pthread_mutex_unlock(&T.lock);

    if (pthread_create(&T.poll_thread, NULL, poll_worker, NULL) != 0) {
        LOGE("telegram: cannot start the command poller: %s -- alerts still "
             "send, but /preview will not answer", strerror(errno));
        pthread_mutex_lock(&T.lock);
        T.poll_run = false;
        T.polling  = false;
        pthread_mutex_unlock(&T.lock);
    }
}

static void tg_poll_stop(void)
{
    pthread_mutex_lock(&T.lock);
    if (!T.poll_run) {
        pthread_mutex_unlock(&T.lock);
        return;
    }
    T.poll_run = false;
    pthread_mutex_unlock(&T.lock);
    /* The progress callback sees poll_run go false within about a second and
     * aborts the in-flight long poll, so this join is quick even though the
     * request it interrupts had 25 seconds left to run. */
    pthread_join(T.poll_thread, NULL);
}
