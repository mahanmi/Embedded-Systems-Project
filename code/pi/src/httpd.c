#include "httpd.h"

#include "api.h"
#include "common.h"
#include "config.h"
#include "log.h"
#include "shmframe.h"
#include "state.h"

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* libmicrohttpd changed the callback return type in 0.9.71+. */
#if MHD_VERSION >= 0x00097002
#  define MHD_RESULT enum MHD_Result
#else
#  define MHD_RESULT int
#endif

static struct MHD_Daemon *D_HTTPS = NULL;
static struct MHD_Daemon *D_HTTP  = NULL;
static struct MHD_Daemon *D_LOOP  = NULL;

static char *TLS_CERT_PEM = NULL;
static char *TLS_KEY_PEM  = NULL;
static char *PAGE_HTML    = NULL;   /* dashboard, placeholders substituted */
static size_t PAGE_LEN    = 0;

#define MJPEG_BOUNDARY "guardianframe"

/* ------------------------------------------------------------- utilities */

static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

/* Replaces every occurrence of `needle`. Returns a new buffer. */
static char *str_replace_all(const char *src, const char *needle, const char *rep)
{
    size_t nlen = strlen(needle), rlen = strlen(rep);
    size_t count = 0;
    for (const char *p = src; (p = strstr(p, needle)); p += nlen)
        count++;

    size_t out_len = strlen(src) + count * (rlen - nlen) + 1;
    char *out = malloc(out_len);
    if (!out)
        return NULL;

    char *w = out;
    const char *p = src;
    while (1) {
        const char *hit = strstr(p, needle);
        if (!hit) {
            strcpy(w, p);
            break;
        }
        size_t chunk = (size_t)(hit - p);
        memcpy(w, p, chunk);
        w += chunk;
        memcpy(w, rep, rlen);
        w += rlen;
        p = hit + nlen;
    }
    return out;
}

/* Constant-time comparison so the bearer token cannot be recovered by
 * timing the 401 responses. */
static bool secure_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0 && la == lb;
}

static bool request_authorised(struct MHD_Connection *conn)
{
    const char *h = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                "Authorization");
    if (!h)
        return false;
    if (strncasecmp(h, "Bearer ", 7) != 0)
        return false;
    return secure_equal(h + 7, g_cfg.api_token);
}

static MHD_RESULT send_bytes(struct MHD_Connection *conn, int status,
                             const char *ctype, const void *data, size_t len,
                             bool free_data)
{
    struct MHD_Response *r = MHD_create_response_from_buffer(
        len, (void *)data,
        free_data ? MHD_RESPMEM_MUST_FREE : MHD_RESPMEM_MUST_COPY);
    if (!r)
        return MHD_NO;

    MHD_add_response_header(r, "Content-Type", ctype);
    MHD_add_response_header(r, "Cache-Control", "no-store, must-revalidate");
    /* Modest hardening for a page that is served over TLS anyway. */
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "Server", "guardian/" GUARDIAN_VERSION);

    MHD_RESULT rc = MHD_queue_response(conn, (unsigned int)status, r);
    MHD_destroy_response(r);
    return rc;
}

static MHD_RESULT send_json(struct MHD_Connection *conn, int status, char *json)
{
    if (!json)
        return send_bytes(conn, 500, "application/json",
                          "{\"error\":\"internal\"}", 20, false);
    return send_bytes(conn, status, "application/json", json, strlen(json), true);
}

/* ------------------------------------------------------- MJPEG streaming */

struct mjpeg_ctx {
    struct guardian_frame frame;     /* ~256 KB, one per streaming client */
    char    head[256];
    size_t  head_len;
    size_t  head_off;
    size_t  body_off;
    bool    have;
    double  last_ts;
    double  started;
};

/* Called by MHD whenever it can write more bytes to this client. Thread-per-
 * connection mode means blocking here only stalls this one client. */
static ssize_t mjpeg_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct mjpeg_ctx *c = cls;

    /* Still emitting the part headers of the current frame. */
    if (c->have && c->head_off < c->head_len) {
        size_t n = c->head_len - c->head_off;
        if (n > max) n = max;
        memcpy(buf, c->head + c->head_off, n);
        c->head_off += n;
        return (ssize_t)n;
    }

    /* Still emitting the JPEG payload of the current frame. */
    if (c->have && c->body_off < c->frame.jpeg_len) {
        size_t n = c->frame.jpeg_len - c->body_off;
        if (n > max) n = max;
        memcpy(buf, c->frame.jpeg + c->body_off, n);
        c->body_off += n;
        return (ssize_t)n;
    }

    /* Frame finished -- wait for the detector to publish the next one. */
    for (int spins = 0; spins < 600; spins++) {  /* up to ~30 s */
        double ts = 0;
        if (shmframe_last_ts(&ts, NULL) && ts > c->last_ts) {
            if (shmframe_read(&c->frame, true) && c->frame.jpeg_len) {
                c->last_ts = c->frame.ts_mono;
                c->head_len = (size_t)snprintf(c->head, sizeof c->head,
                    "\r\n--" MJPEG_BOUNDARY "\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %u\r\n"
                    "X-Persons: %d\r\n"
                    "X-FPS: %.2f\r\n"
                    "X-Timestamp: %.3f\r\n\r\n",
                    c->frame.jpeg_len, c->frame.persons, c->frame.fps,
                    c->frame.ts_wall);
                c->head_off = 0;
                c->body_off = 0;
                c->have = true;
                return mjpeg_cb(cls, pos, buf, max);
            }
        }
        usleep(50000); /* 50 ms; the detector runs at a few FPS at best */
    }

    /* No frames for 30 s: end the response so the browser's onerror fires and
     * the page re-attaches, rather than holding a dead socket open. */
    LOGW("stream: no frames for 30 s, closing client");
    return MHD_CONTENT_READER_END_OF_STREAM;
}

static void mjpeg_free(void *cls) { free(cls); }

static MHD_RESULT handle_stream(struct MHD_Connection *conn)
{
    struct mjpeg_ctx *c = calloc(1, sizeof *c);
    if (!c)
        return send_json(conn, 500, strdup("{\"error\":\"out of memory\"}"));
    c->started = now_mono();

    /* 32 KB write blocks keep the syscall count sane without pinning much
     * memory per client. */
    struct MHD_Response *r = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 32 * 1024, mjpeg_cb, c, mjpeg_free);
    if (!r) {
        free(c);
        return MHD_NO;
    }

    MHD_add_response_header(r, "Content-Type",
                            "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY);
    MHD_add_response_header(r, "Cache-Control", "no-store, no-cache, must-revalidate");
    MHD_add_response_header(r, "Pragma", "no-cache");
    MHD_add_response_header(r, "Connection", "close");

    MHD_RESULT rc = MHD_queue_response(conn, MHD_HTTP_OK, r);
    MHD_destroy_response(r);
    return rc;
}

/* ------------------------------------------------------------- 301 redirect */

static MHD_RESULT handle_redirect(struct MHD_Connection *conn, const char *url)
{
    /* Prefer the Host header the client actually used, so the redirect works
     * whether the board was reached by IP, by .local name, or by hostname. */
    const char *host = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Host");
    char hostbuf[192];
    if (host && *host) {
        snprintf(hostbuf, sizeof hostbuf, "%s", host);
        char *colon = strrchr(hostbuf, ':');   /* strip the :80 */
        if (colon && !strchr(hostbuf, '['))
            *colon = '\0';
    } else {
        snprintf(hostbuf, sizeof hostbuf, "%s", g_cfg.public_host);
    }

    char location[512];
    if (g_cfg.https_port == 443)
        snprintf(location, sizeof location, "https://%s%s", hostbuf, url);
    else
        snprintf(location, sizeof location, "https://%s:%d%s", hostbuf,
                 g_cfg.https_port, url);

    static const char BODY[] =
        "<!doctype html><meta charset=utf-8>"
        "<title>301 Moved Permanently</title>"
        "<h1>301 Moved Permanently</h1>"
        "<p>This service is available over HTTPS only.</p>";

    struct MHD_Response *r = MHD_create_response_from_buffer(
        sizeof BODY - 1, (void *)BODY, MHD_RESPMEM_PERSISTENT);
    if (!r)
        return MHD_NO;

    MHD_add_response_header(r, "Location", location);
    MHD_add_response_header(r, "Content-Type", "text/html; charset=utf-8");
    /* HSTS: once a browser has seen this it will not even try plaintext. */
    MHD_add_response_header(r, "Strict-Transport-Security", "max-age=31536000");

    MHD_RESULT rc = MHD_queue_response(conn, MHD_HTTP_MOVED_PERMANENTLY, r);
    MHD_destroy_response(r);
    return rc;
}

/* ------------------------------------------------------------------ router */

struct conn_ctx {
    char  *body;
    size_t body_len;
    size_t body_cap;
};

static int query_int(struct MHD_Connection *conn, const char *key, int fallback)
{
    const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, key);
    if (!v || !*v)
        return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

static MHD_RESULT route(void *cls, struct MHD_Connection *conn, const char *url,
                        const char *method, const char *version,
                        const char *upload_data, size_t *upload_data_size,
                        void **con_cls)
{
    (void)version;
    const bool is_redirector = (cls != NULL);

    /* First call for a request: MHD wants us to set up per-connection state
     * and return without queueing anything. */
    if (*con_cls == NULL) {
        struct conn_ctx *c = calloc(1, sizeof *c);
        if (!c)
            return MHD_NO;
        *con_cls = c;
        return MHD_YES;
    }
    struct conn_ctx *ctx = *con_cls;

    /* Accumulate POST/PUT bodies across however many calls MHD needs. */
    if (*upload_data_size) {
        size_t need = ctx->body_len + *upload_data_size + 1;
        if (need > 64 * 1024) {           /* nothing legitimate is this big */
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (need > ctx->body_cap) {
            size_t cap = ctx->body_cap ? ctx->body_cap * 2 : 1024;
            while (cap < need) cap *= 2;
            char *nb = realloc(ctx->body, cap);
            if (!nb)
                return MHD_NO;
            ctx->body = nb;
            ctx->body_cap = cap;
        }
        memcpy(ctx->body + ctx->body_len, upload_data, *upload_data_size);
        ctx->body_len += *upload_data_size;
        ctx->body[ctx->body_len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    state_inc_http();

    /* The :80 daemon never serves content. */
    if (is_redirector)
        return handle_redirect(conn, url);

    const bool is_get  = !strcmp(method, "GET");
    const bool is_post = !strcmp(method, "POST");
    const bool is_head = !strcmp(method, "HEAD");
    int status = 200;

    /* ---- static page ---- */
    if ((is_get || is_head) && (!strcmp(url, "/") || !strcmp(url, "/index.html")))
        return send_bytes(conn, 200, "text/html; charset=utf-8",
                          PAGE_HTML, PAGE_LEN, false);

    if (is_get && !strcmp(url, "/favicon.ico"))
        return send_bytes(conn, 204, "image/x-icon", "", 0, false);

    /* ---- API ---- */
    if (!strncmp(url, "/api/v1/", 8)) {
        const char *ep = url + 8;
        char *body = NULL;

        /* The stream is the one endpoint that does not return a JSON body. */
        if (is_get && !strcmp(ep, "stream"))
            return handle_stream(conn);

        if (is_get && !strcmp(ep, "persons"))          body = api_persons(&status);
        else if (is_get && !strcmp(ep, "telemetry"))   body = api_telemetry(&status);
        else if (is_get && !strcmp(ep, "history"))     body = api_history(query_int(conn, "limit", 5), &status);
        else if (is_get && !strcmp(ep, "guard"))       body = api_guard_get(&status);
        else if (is_post && !strcmp(ep, "guard"))      body = api_guard_set(ctx->body, &status);
        else if (is_get && !strcmp(ep, "stats"))       body = api_stats(&status);
        else if (is_get && !strcmp(ep, "health"))      body = api_health(&status);
        else if (is_get && !strcmp(ep, "commands"))    body = api_commands(&status);
        else if (is_post && !strcmp(ep, "command"))
            body = api_command(ctx->body, request_authorised(conn), &status);
        else
            body = api_error("not_found", "no such endpoint", &status, 404);

        return send_json(conn, status, body);
    }

    char *nf = api_error("not_found", "no such resource", &status, 404);
    return send_json(conn, status, nf);
}

static void request_done(void *cls, struct MHD_Connection *conn,
                         void **con_cls, enum MHD_RequestTerminationCode toe)
{
    (void)cls; (void)conn; (void)toe;
    struct conn_ctx *ctx = *con_cls;
    if (ctx) {
        free(ctx->body);
        free(ctx);
        *con_cls = NULL;
    }
}

/* ------------------------------------------------------------------ setup */

static bool load_page(void)
{
    size_t len = 0;
    char *raw = read_file("/opt/guardian/www/index.html", &len);
    if (!raw) {
        LOGW("httpd: /opt/guardian/www/index.html missing, serving a stub page");
        raw = strdup("<!doctype html><meta charset=utf-8>"
                     "<title>__STUDENT_NAME__ &mdash; __STUDENT_ID__</title>"
                     "<h1>Smart Guardian System</h1>"
                     "<p>__STUDENT_NAME__ &mdash; __STUDENT_ID__</p>"
                     "<img src=\"/api/v1/stream\">");
        if (!raw)
            return false;
    }

    char swagger[160];
    snprintf(swagger, sizeof swagger, "https://%s:8443/docs", g_cfg.public_host);

    char *a = str_replace_all(raw, "__STUDENT_NAME__", g_cfg.student_name);
    free(raw);
    if (!a) return false;
    char *b = str_replace_all(a, "__STUDENT_ID__", g_cfg.student_id);
    free(a);
    if (!b) return false;
    char *c = str_replace_all(b, "__SWAGGER_URL__", swagger);
    free(b);
    if (!c) return false;

    PAGE_HTML = c;
    PAGE_LEN = strlen(c);
    LOGI("httpd: dashboard page ready (%zu bytes)", PAGE_LEN);
    return true;
}

bool httpd_start(void)
{
    if (!load_page())
        return false;

    size_t clen = 0, klen = 0;
    TLS_CERT_PEM = read_file(g_cfg.tls_cert, &clen);
    TLS_KEY_PEM  = read_file(g_cfg.tls_key, &klen);
    if (!TLS_CERT_PEM || !TLS_KEY_PEM) {
        LOGE("httpd: cannot read TLS material (%s / %s) -- run scripts/gen_cert.sh",
             g_cfg.tls_cert, g_cfg.tls_key);
        return false;
    }

    /* Thread-per-connection: the MJPEG handler blocks while it waits for the
     * next frame, which would stall a shared thread pool. Stacks are capped
     * so 50 simultaneous clients (experiment 2-3) stay affordable. */
    const unsigned FLAGS = MHD_USE_THREAD_PER_CONNECTION |
                           MHD_USE_INTERNAL_POLLING_THREAD |
                           MHD_USE_ERROR_LOG;

    D_HTTPS = MHD_start_daemon(
        FLAGS | MHD_USE_TLS, (uint16_t)g_cfg.https_port, NULL, NULL,
        &route, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, TLS_CERT_PEM,
        MHD_OPTION_HTTPS_MEM_KEY,  TLS_KEY_PEM,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned)128,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)120,
        MHD_OPTION_THREAD_STACK_SIZE, (size_t)(512 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, &request_done, NULL,
        MHD_OPTION_END);
    if (!D_HTTPS) {
        LOGE("httpd: cannot bind https port %d", g_cfg.https_port);
        return false;
    }
    LOGI("httpd: HTTPS listening on :%d (TLS, CN=%s)", g_cfg.https_port,
         g_cfg.student_id);

    /* cls != NULL marks this daemon as the redirector. */
    static int redirector_marker = 1;
    D_HTTP = MHD_start_daemon(
        FLAGS, (uint16_t)g_cfg.http_port, NULL, NULL,
        &route, &redirector_marker,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned)64,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)15,
        MHD_OPTION_THREAD_STACK_SIZE, (size_t)(256 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, &request_done, NULL,
        MHD_OPTION_END);
    if (!D_HTTP)
        LOGE("httpd: cannot bind http port %d -- the 301 redirect is unavailable",
             g_cfg.http_port);
    else
        LOGI("httpd: HTTP listening on :%d (301 -> https)", g_cfg.http_port);

    /* Loopback-only plaintext listener for the FastAPI gateway. */
    struct sockaddr_in loop = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_cfg.loopback_port),
        .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
    };
    D_LOOP = MHD_start_daemon(
        FLAGS, 0, NULL, NULL,
        &route, NULL,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&loop,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned)64,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned)120,
        MHD_OPTION_THREAD_STACK_SIZE, (size_t)(512 * 1024),
        MHD_OPTION_NOTIFY_COMPLETED, &request_done, NULL,
        MHD_OPTION_END);
    if (!D_LOOP)
        LOGE("httpd: cannot bind loopback port %d", g_cfg.loopback_port);
    else
        LOGI("httpd: loopback API on 127.0.0.1:%d (for the Swagger gateway)",
             g_cfg.loopback_port);

    return true;
}

void httpd_stop(void)
{
    if (D_HTTPS) { MHD_stop_daemon(D_HTTPS); D_HTTPS = NULL; }
    if (D_HTTP)  { MHD_stop_daemon(D_HTTP);  D_HTTP  = NULL; }
    if (D_LOOP)  { MHD_stop_daemon(D_LOOP);  D_LOOP  = NULL; }
    free(TLS_CERT_PEM); TLS_CERT_PEM = NULL;
    free(TLS_KEY_PEM);  TLS_KEY_PEM  = NULL;
    free(PAGE_HTML);    PAGE_HTML = NULL;
    LOGI("httpd: stopped");
}
