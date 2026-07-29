#include "sysinfo.h"

#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CORES 8

/* Cumulative jiffy counters from one /proc/stat line. */
struct cpu_times {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
};

static struct {
    pthread_mutex_t lock;
    struct cpu_times prev_total;
    struct cpu_times prev_core[MAX_CORES];
    int ncores;
    struct sysinfo_snapshot snap;
    bool primed;
} S = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ------------------------------------------------------------------ helpers */

/* Read a whole small pseudo-file. Returns bytes read, or -1. */
static ssize_t slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (ssize_t)n;
}

static bool read_u64_file(const char *path, uint64_t *out)
{
    char buf[64];
    if (slurp(path, buf, sizeof buf) < 0)
        return false;
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf)
        return false;
    *out = (uint64_t)v;
    return true;
}

static uint64_t busy_of(const struct cpu_times *t)
{
    return t->user + t->nice + t->system + t->irq + t->softirq + t->steal;
}

static uint64_t total_of(const struct cpu_times *t)
{
    return busy_of(t) + t->idle + t->iowait;
}

static double pct_delta(const struct cpu_times *now, const struct cpu_times *prev)
{
    uint64_t tot_d  = total_of(now) - total_of(prev);
    uint64_t busy_d = busy_of(now)  - busy_of(prev);
    if (tot_d == 0)
        return 0.0;
    double p = 100.0 * (double)busy_d / (double)tot_d;
    if (p < 0.0)   p = 0.0;
    if (p > 100.0) p = 100.0;
    return p;
}

/* ------------------------------------------------------------- /sys thermal */

double sysinfo_cpu_temp(void)
{
    uint64_t milli;
    if (!read_u64_file("/sys/class/thermal/thermal_zone0/temp", &milli))
        return -1.0;
    return (double)milli / 1000.0;
}

/* -------------------------------------------------------- /proc/self/statm */

uint64_t sysinfo_self_rss_kb(void)
{
    char buf[128];
    if (slurp("/proc/self/statm", buf, sizeof buf) < 0)
        return 0;
    /* fields: size resident shared text lib data dt -- all in pages */
    unsigned long long size = 0, resident = 0;
    if (sscanf(buf, "%llu %llu", &size, &resident) != 2)
        return 0;
    (void)size;
    long pagesz = sysconf(_SC_PAGESIZE);
    return (uint64_t)resident * (uint64_t)pagesz / 1024u;
}

/* --------------------------------------------------------------- /proc/stat */

static int read_proc_stat(struct cpu_times *total, struct cpu_times core[MAX_CORES])
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return 0;

    char line[256];
    int ncores = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "cpu", 3) != 0)
            break; /* cpu lines come first */

        struct cpu_times t = { 0 };
        static const char *FMT = "%" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
                                 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64;
        if (line[3] == ' ') {
            sscanf(line + 4, FMT, &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            if (total)
                *total = t;
        } else {
            int idx = atoi(line + 3);
            const char *sp = strchr(line, ' ');
            if (!sp || idx < 0 || idx >= MAX_CORES)
                continue;
            sscanf(sp + 1, FMT, &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal);
            if (core)
                core[idx] = t;
            if (idx + 1 > ncores)
                ncores = idx + 1;
        }
    }
    fclose(f);
    return ncores;
}

/* ----------------------------------------------------------- /proc/meminfo */

static void read_meminfo(struct sysinfo_snapshot *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return;

    char key[64];
    unsigned long long val;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63[^:]: %llu kB", key, &val) != 2)
            continue;
        if      (!strcmp(key, "MemTotal"))     s->mem_total_kb     = val;
        else if (!strcmp(key, "MemFree"))      s->mem_free_kb      = val;
        else if (!strcmp(key, "MemAvailable")) s->mem_available_kb = val;
        else if (!strcmp(key, "Buffers"))      s->mem_buffers_kb   = val;
        else if (!strcmp(key, "Cached"))       s->mem_cached_kb    = val;
        else if (!strcmp(key, "SwapTotal"))    s->swap_total_kb    = val;
        else if (!strcmp(key, "SwapFree"))     s->swap_free_kb     = val;
    }
    fclose(f);

    /* "Used" is defined against MemAvailable rather than MemFree: on Linux the
     * page cache is reclaimable, so MemFree alone dramatically overstates
     * pressure on a board that has been streaming video for a while. */
    if (s->mem_total_kb)
        s->mem_used_percent =
            100.0 * (double)(s->mem_total_kb - s->mem_available_kb) / (double)s->mem_total_kb;
}

/* ------------------------------------------------------- uptime / loadavg */

static void read_uptime_load(struct sysinfo_snapshot *s)
{
    char buf[128];
    if (slurp("/proc/uptime", buf, sizeof buf) > 0)
        sscanf(buf, "%lf", &s->uptime_sec);
    if (slurp("/proc/loadavg", buf, sizeof buf) > 0)
        sscanf(buf, "%lf %lf %lf", &s->load1, &s->load5, &s->load15);
}

static void read_cpufreq(struct sysinfo_snapshot *s)
{
    uint64_t khz;
    if (read_u64_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &khz))
        s->cpu_freq_khz = khz;
}

/* -------------------------------------------------------------------- API */

void sysinfo_init(void)
{
    pthread_mutex_lock(&S.lock);
    S.ncores = read_proc_stat(&S.prev_total, S.prev_core);
    if (S.ncores > MAX_CORES)
        S.ncores = MAX_CORES;
    S.snap.ncores = S.ncores;
    S.primed = false;
    pthread_mutex_unlock(&S.lock);
    LOGI("sysinfo: %d cores, thermal zone reads %.1f C", S.ncores, sysinfo_cpu_temp());
}

void sysinfo_sample(void)
{
    struct cpu_times total = { 0 };
    struct cpu_times core[MAX_CORES] = { { 0 } };
    int ncores = read_proc_stat(&total, core);
    if (ncores > MAX_CORES)
        ncores = MAX_CORES;

    struct sysinfo_snapshot s;
    memset(&s, 0, sizeof s);
    s.ncores = ncores;
    s.cpu_temp_c = sysinfo_cpu_temp();
    read_meminfo(&s);
    read_uptime_load(&s);
    read_cpufreq(&s);
    s.self_rss_kb = sysinfo_self_rss_kb();

    pthread_mutex_lock(&S.lock);
    s.cpu_percent = pct_delta(&total, &S.prev_total);
    for (int i = 0; i < ncores; i++)
        s.cpu_percent_per_core[i] = pct_delta(&core[i], &S.prev_core[i]);

    /* The very first delta spans the whole uptime, which is meaningless;
     * report 0 until we have two real samples. */
    if (!S.primed) {
        s.cpu_percent = 0.0;
        for (int i = 0; i < ncores; i++)
            s.cpu_percent_per_core[i] = 0.0;
        S.primed = true;
    }

    S.prev_total = total;
    memcpy(S.prev_core, core, sizeof core);
    S.snap = s;
    pthread_mutex_unlock(&S.lock);
}

void sysinfo_get(struct sysinfo_snapshot *out)
{
    pthread_mutex_lock(&S.lock);
    *out = S.snap;
    pthread_mutex_unlock(&S.lock);
}
