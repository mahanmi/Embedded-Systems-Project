#include "shmframe.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static struct guardian_shm *g_shm = NULL;
static int g_fd = -1;

bool shmframe_open(bool create)
{
    int flags = create ? (O_RDWR | O_CREAT) : O_RDWR;
    g_fd = shm_open(GUARDIAN_SHM_NAME, flags, 0660);
    if (g_fd < 0) {
        LOGE("shm_open(%s): %s", GUARDIAN_SHM_NAME, strerror(errno));
        return false;
    }

    if (create) {
        if (ftruncate(g_fd, (off_t)sizeof(struct guardian_shm)) != 0) {
            LOGE("ftruncate shm: %s", strerror(errno));
            close(g_fd);
            g_fd = -1;
            return false;
        }
    }

    void *p = mmap(NULL, sizeof(struct guardian_shm), PROT_READ | PROT_WRITE,
                   MAP_SHARED, g_fd, 0);
    if (p == MAP_FAILED) {
        LOGE("mmap shm: %s", strerror(errno));
        close(g_fd);
        g_fd = -1;
        return false;
    }
    g_shm = (struct guardian_shm *)p;

    if (create && g_shm->magic != GUARDIAN_SHM_MAGIC) {
        memset(g_shm, 0, sizeof *g_shm);
        g_shm->magic = GUARDIAN_SHM_MAGIC;
        g_shm->version = GUARDIAN_SHM_VER;
        g_shm->active = (uint32_t)-1; /* nothing published yet */
    }

    LOGI("shared frame segment ready (%zu KiB, %u slots)",
         sizeof(struct guardian_shm) / 1024, GUARDIAN_NSLOTS);
    return true;
}

void shmframe_close(void)
{
    if (g_shm) {
        munmap(g_shm, sizeof(struct guardian_shm));
        g_shm = NULL;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/* Read side of the seqlock. `copy` performs the payload copy under the
 * sequence guard; if it tears we retry a bounded number of times. */
static bool read_slot(struct guardian_frame *out, bool want_jpeg)
{
    if (!g_shm || g_shm->magic != GUARDIAN_SHM_MAGIC)
        return false;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t idx = __atomic_load_n(&g_shm->active, __ATOMIC_ACQUIRE);
        if (idx >= GUARDIAN_NSLOTS)
            return false; /* detector has not published anything yet */

        const struct guardian_slot *s = &g_shm->slot[idx];

        uint64_t s1 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)
            continue; /* writer is mid-update */

        out->ts_wall  = s->ts_wall;
        out->ts_mono  = s->ts_mono;
        out->persons  = s->persons;
        out->fps      = s->fps;
        out->width    = s->width;
        out->height   = s->height;
        out->infer_ms = s->infer_ms;
        out->flags    = s->flags;

        uint32_t dlen = s->det_len;
        if (dlen >= GUARDIAN_DETJSON_MAX)
            dlen = GUARDIAN_DETJSON_MAX - 1;
        memcpy(out->det, s->det, dlen);
        out->det[dlen] = '\0';

        uint32_t jlen = s->jpeg_len;
        if (jlen > GUARDIAN_JPEG_MAX)
            jlen = GUARDIAN_JPEG_MAX;
        out->jpeg_len = jlen;
        if (want_jpeg && jlen)
            memcpy(out->jpeg, s->jpeg, jlen);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint64_t s2 = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if (s1 == s2)
            return true; /* stable snapshot */
    }
    return false; /* writer is spinning far faster than us -- give up */
}

bool shmframe_read(struct guardian_frame *out, bool want_jpeg)
{
    memset(out, 0, want_jpeg ? sizeof *out : offsetof(struct guardian_frame, jpeg));
    return read_slot(out, want_jpeg);
}

bool shmframe_last_ts(double *ts_mono, double *ts_wall)
{
    if (!g_shm)
        return false;
    uint32_t idx = __atomic_load_n(&g_shm->active, __ATOMIC_ACQUIRE);
    if (idx >= GUARDIAN_NSLOTS)
        return false;
    if (ts_mono) *ts_mono = g_shm->slot[idx].ts_mono;
    if (ts_wall) *ts_wall = g_shm->slot[idx].ts_wall;
    return true;
}

bool shmframe_last_real_ts(double *ts_mono)
{
    if (!g_shm)
        return false;
    double t = g_shm->last_real_ts;
    if (t <= 0.0)
        return false;
    if (ts_mono)
        *ts_mono = t;
    return true;
}

int shmframe_persons(void)
{
    if (!g_shm)
        return -1;
    uint32_t idx = __atomic_load_n(&g_shm->active, __ATOMIC_ACQUIRE);
    if (idx >= GUARDIAN_NSLOTS)
        return -1;
    return g_shm->slot[idx].persons;
}

uint64_t shmframe_frames_total(void)
{
    return g_shm ? __atomic_load_n(&g_shm->frames_total, __ATOMIC_RELAXED) : 0;
}

void shmframe_set_target(int fps, int width, int net_input)
{
    if (!g_shm)
        return;
    if (fps > 0)
        __atomic_store_n(&g_shm->target_fps, fps, __ATOMIC_RELEASE);
    if (width > 0)
        __atomic_store_n(&g_shm->target_width, width, __ATOMIC_RELEASE);
    /* SHMFRAME_NET_OFF must get through: it is a mode, not a size, and 0 is
     * already spoken for as "leave unchanged". */
    if (net_input > 0 || net_input == SHMFRAME_NET_OFF)
        __atomic_store_n(&g_shm->target_net_input, net_input, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_shm->ctrl_seq, 1, __ATOMIC_RELEASE);

    int f = 0, w = 0, n = 0;
    shmframe_get_target(&f, &w, &n);
    LOGI("control: target_fps=%d target_width=%d net_input=%d", f, w, n);
}

void shmframe_get_target(int *fps, int *width, int *net_input)
{
    if (!g_shm) {
        if (fps) *fps = 0;
        if (width) *width = 0;
        if (net_input) *net_input = 0;
        return;
    }
    if (fps)   *fps   = __atomic_load_n(&g_shm->target_fps, __ATOMIC_ACQUIRE);
    if (width) *width = __atomic_load_n(&g_shm->target_width, __ATOMIC_ACQUIRE);
    if (net_input)
        *net_input = __atomic_load_n(&g_shm->target_net_input, __ATOMIC_ACQUIRE);
}
