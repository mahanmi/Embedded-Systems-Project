/* =========================================================================
 *  System telemetry
 *
 *  The project brief is explicit that temperature and memory must be read
 *  straight out of the kernel's virtual filesystems from C -- shelling out to
 *  `vcgencmd`, `free`, `top` or `sensors` earns no credit. Every value below
 *  therefore comes from parsing one of:
 *
 *      /sys/class/thermal/thermal_zone0/temp   SoC temperature, milli-degrees
 *      /proc/meminfo                           memory, kB
 *      /proc/stat                              cumulative CPU jiffies
 *      /proc/uptime                            seconds since boot
 *      /proc/loadavg                           1/5/15 minute load
 *      /proc/self/statm                        this process's own RSS
 *      /sys/devices/system/cpu/.../scaling_cur_freq
 *
 *  CPU utilisation is inherently a rate, so it is computed as a delta between
 *  two /proc/stat samples; sysinfo_init() takes the first one and a background
 *  sampler refreshes it once a second.
 * ========================================================================= */
#ifndef GUARDIAN_SYSINFO_H
#define GUARDIAN_SYSINFO_H

#include <stdbool.h>
#include <stdint.h>

struct sysinfo_snapshot {
    double   cpu_temp_c;      /* -1 when the thermal zone is unreadable      */
    double   cpu_percent;     /* busy % across all cores since last sample   */
    double   cpu_percent_per_core[8];
    int      ncores;
    uint64_t mem_total_kb;
    uint64_t mem_free_kb;
    uint64_t mem_available_kb;
    uint64_t mem_buffers_kb;
    uint64_t mem_cached_kb;
    double   mem_used_percent; /* based on available, i.e. what apps can get  */
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    double   uptime_sec;
    double   load1, load5, load15;
    uint64_t cpu_freq_khz;
    uint64_t self_rss_kb;      /* this daemon's resident set                 */
};

/* Takes the baseline /proc/stat sample. */
void sysinfo_init(void);

/* Re-reads /proc/stat and updates the utilisation figures. Call ~1 Hz. */
void sysinfo_sample(void);

/* Fills `out` with the most recent values. Safe from any thread. */
void sysinfo_get(struct sysinfo_snapshot *out);

/* Direct single-value reads, used on hot paths that only need one number. */
double sysinfo_cpu_temp(void);
uint64_t sysinfo_self_rss_kb(void);

#endif /* GUARDIAN_SYSINFO_H */
