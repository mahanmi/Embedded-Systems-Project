#include "store.h"

#include "common.h"
#include "log.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *DB = NULL;
static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;
static int RING = 1000;

static bool exec_or_log(const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(DB, sql, NULL, NULL, &err) != SQLITE_OK) {
        LOGE("sqlite: %s", err ? err : "(unknown)");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool store_open(const char *path, int ring_size)
{
    RING = ring_size > 0 ? ring_size : 1000;

    if (sqlite3_open(path, &DB) != SQLITE_OK) {
        LOGE("sqlite open %s: %s", path, sqlite3_errmsg(DB));
        return false;
    }

    /* WAL keeps readers (the API) from blocking the writer (the detection
     * consumer); the busy timeout covers the brief checkpoint windows. */
    exec_or_log("PRAGMA journal_mode=WAL;");
    exec_or_log("PRAGMA synchronous=NORMAL;");
    sqlite3_busy_timeout(DB, 3000);

    if (!exec_or_log(
            "CREATE TABLE IF NOT EXISTS detections("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  ts_wall    REAL    NOT NULL,"
            "  persons    INTEGER NOT NULL,"
            "  cpu_temp_c REAL,"
            "  fps        REAL,"
            "  guard_mode INTEGER NOT NULL DEFAULT 0,"
            "  emailed    INTEGER NOT NULL DEFAULT 0,"
            "  note       TEXT"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_det_ts ON detections(ts_wall);"
            "CREATE TABLE IF NOT EXISTS stats("
            "  k TEXT PRIMARY KEY,"
            "  v REAL NOT NULL DEFAULT 0"
            ");"
            "INSERT OR IGNORE INTO stats(k,v) VALUES"
            "  ('total_events',0),('total_persons',0),('peak_persons',0);"))
        return false;

    /* Ring buffer. Recreated on every open so a changed db_ring_size in the
     * configuration takes effect without a manual migration. */
    char sql[512];
    snprintf(sql, sizeof sql,
             "DROP TRIGGER IF EXISTS trg_detections_ring;"
             "CREATE TRIGGER trg_detections_ring AFTER INSERT ON detections "
             "BEGIN "
             "  DELETE FROM detections WHERE id <= NEW.id - %d; "
             "END;",
             RING);
    if (!exec_or_log(sql))
        return false;

    /* Trim anything left over from a previous, larger ring. */
    snprintf(sql, sizeof sql,
             "DELETE FROM detections WHERE id <= "
             "(SELECT IFNULL(MAX(id),0) FROM detections) - %d;", RING);
    exec_or_log(sql);

    LOGI("store: %s open (ring=%d rows)", path, RING);
    return true;
}

void store_close(void)
{
    pthread_mutex_lock(&L);
    if (DB) {
        sqlite3_close(DB);
        DB = NULL;
    }
    pthread_mutex_unlock(&L);
}

bool store_add_detection(double ts_wall, int persons, double cpu_temp_c,
                         double fps, bool guard_mode, bool emailed,
                         const char *note)
{
    if (!DB)
        return false;

    pthread_mutex_lock(&L);
    bool ok = false;

    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT INTO detections"
                      "(ts_wall,persons,cpu_temp_c,fps,guard_mode,emailed,note)"
                      " VALUES(?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(DB, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_double(st, 1, ts_wall);
        sqlite3_bind_int(st, 2, persons);
        sqlite3_bind_double(st, 3, cpu_temp_c);
        sqlite3_bind_double(st, 4, fps);
        sqlite3_bind_int(st, 5, guard_mode ? 1 : 0);
        sqlite3_bind_int(st, 6, emailed ? 1 : 0);
        sqlite3_bind_text(st, 7, note ? note : "", -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(st) == SQLITE_DONE);
        if (!ok)
            LOGE("sqlite insert: %s", sqlite3_errmsg(DB));
    } else {
        LOGE("sqlite prepare: %s", sqlite3_errmsg(DB));
    }
    sqlite3_finalize(st);

    if (ok) {
        /* Lifetime aggregates, deliberately outside the ring. */
        char sql2[320];
        snprintf(sql2, sizeof sql2,
                 "UPDATE stats SET v=v+1 WHERE k='total_events';"
                 "UPDATE stats SET v=v+%d WHERE k='total_persons';"
                 "UPDATE stats SET v=MAX(v,%d) WHERE k='peak_persons';",
                 persons, persons);
        char *err = NULL;
        if (sqlite3_exec(DB, sql2, NULL, NULL, &err) != SQLITE_OK) {
            LOGE("sqlite stats: %s", err ? err : "?");
            sqlite3_free(err);
        }
    }
    pthread_mutex_unlock(&L);
    return ok;
}

int store_recent(struct detection_row *out, int max_rows)
{
    if (!DB || max_rows <= 0)
        return 0;

    pthread_mutex_lock(&L);
    int n = 0;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT id,ts_wall,persons,cpu_temp_c,fps,guard_mode,"
                      "emailed,IFNULL(note,'') FROM detections "
                      "ORDER BY id DESC LIMIT ?;";
    if (sqlite3_prepare_v2(DB, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, max_rows);
        while (n < max_rows && sqlite3_step(st) == SQLITE_ROW) {
            struct detection_row *r = &out[n++];
            r->id         = sqlite3_column_int64(st, 0);
            r->ts_wall    = sqlite3_column_double(st, 1);
            r->persons    = sqlite3_column_int(st, 2);
            r->cpu_temp_c = sqlite3_column_double(st, 3);
            r->fps        = sqlite3_column_double(st, 4);
            r->guard_mode = sqlite3_column_int(st, 5);
            r->emailed    = sqlite3_column_int(st, 6);
            snprintf(r->note, sizeof r->note, "%s",
                     (const char *)sqlite3_column_text(st, 7));
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&L);
    return n;
}

static double stat_get(const char *k)
{
    double v = 0.0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(DB, "SELECT v FROM stats WHERE k=?;", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    return v;
}

bool store_stats(uint64_t *events, uint64_t *persons_sum, int *peak_persons,
                 int64_t *rows_now, double *first_ts, double *last_ts)
{
    if (!DB)
        return false;

    pthread_mutex_lock(&L);
    if (events)       *events       = (uint64_t)stat_get("total_events");
    if (persons_sum)  *persons_sum  = (uint64_t)stat_get("total_persons");
    if (peak_persons) *peak_persons = (int)stat_get("peak_persons");

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(DB,
            "SELECT COUNT(*),IFNULL(MIN(ts_wall),0),IFNULL(MAX(ts_wall),0) "
            "FROM detections;", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        if (rows_now) *rows_now = sqlite3_column_int64(st, 0);
        if (first_ts) *first_ts = sqlite3_column_double(st, 1);
        if (last_ts)  *last_ts  = sqlite3_column_double(st, 2);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&L);
    return true;
}

char *store_recent_json(int limit)
{
    if (limit <= 0)   limit = 5;
    if (limit > 200)  limit = 200;

    struct detection_row *rows = calloc((size_t)limit, sizeof *rows);
    if (!rows)
        return NULL;
    int n = store_recent(rows, limit);

    size_t cap = (size_t)n * 256 + 64;
    char *buf = malloc(cap);
    if (!buf) {
        free(rows);
        return NULL;
    }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "[");
    for (int i = 0; i < n; i++) {
        char iso[40];
        iso8601_utc(rows[i].ts_wall, iso, sizeof iso);
        off += (size_t)snprintf(buf + off, cap - off,
                 "%s{\"id\":%lld,\"timestamp\":\"%s\",\"epoch\":%.3f,"
                 "\"persons\":%d,\"cpu_temp_c\":%.1f,\"fps\":%.2f,"
                 "\"guard_mode\":%s,\"emailed\":%s,\"note\":\"%s\"}",
                 i ? "," : "", (long long)rows[i].id, iso, rows[i].ts_wall,
                 rows[i].persons, rows[i].cpu_temp_c, rows[i].fps,
                 rows[i].guard_mode ? "true" : "false",
                 rows[i].emailed ? "true" : "false", rows[i].note);
        if (off >= cap - 1)
            break;
    }
    snprintf(buf + off, cap - off, "]");

    free(rows);
    return buf;
}
