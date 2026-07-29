/* =========================================================================
 *  Shared-memory frame transport
 *
 *  The image-processing stage (Python/OpenCV -- the one component the project
 *  brief allows to be written in Python) hands annotated JPEG frames to the C
 *  daemon through a POSIX shared-memory segment rather than a socket or a
 *  pipe. Reasons:
 *
 *    * zero-copy-ish: one memcpy instead of a full serialise/deserialise per
 *      frame, which matters on a 4-core Cortex-A53;
 *    * the reader never blocks the writer, so a slow HTTP client cannot stall
 *      the detector;
 *    * it doubles as the control channel: the C daemon writes target FPS and
 *      resolution into the header and the detector picks them up, which is how
 *      the adaptive thermal governor (part 4-4) throttles the pipeline.
 *
 *  Concurrency is a classic seqlock over a double buffer: exactly one writer
 *  (the detector) and any number of readers (HTTP stream handlers, the
 *  detection consumer, the watchdog). A reader that observes a torn or
 *  in-progress slot simply retries.
 *
 *  The binary layout below is mirrored byte-for-byte by vision/shmframe.py.
 *  Every field sits at its natural alignment, so the C struct and the Python
 *  `struct` format string with '<' (little-endian, no padding) agree; the
 *  _Static_asserts at the bottom make any divergence a compile error.
 * ========================================================================= */
#ifndef GUARDIAN_SHMFRAME_H
#define GUARDIAN_SHMFRAME_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define GUARDIAN_SHM_NAME  "/guardian_frame"
#define GUARDIAN_SHM_MAGIC 0x31415547u /* "GUA1" little-endian */
#define GUARDIAN_SHM_VER   1u

#define GUARDIAN_JPEG_MAX  (256u * 1024u)
#define GUARDIAN_NSLOTS    2u
#define GUARDIAN_DETJSON_MAX 1024u /* bounding boxes, as a small JSON array */

/* slot flags */
#define GUARDIAN_FRAME_SYNTHETIC 0x1u /* placeholder, no real capture input */

/* Slot header is padded to 64 bytes so the payload starts on a cache line. */
struct guardian_slot {
    uint64_t seq;          /*  0: seqlock, odd => write in progress      */
    double   ts_wall;      /*  8: CLOCK_REALTIME at capture              */
    double   ts_mono;      /* 16: CLOCK_MONOTONIC at capture             */
    uint32_t jpeg_len;     /* 24 */
    int32_t  persons;      /* 28 */
    float    fps;          /* 32: measured, not configured               */
    int32_t  width;        /* 36 */
    int32_t  height;       /* 40 */
    float    infer_ms;     /* 44: inference time for this frame          */
    uint32_t det_len;      /* 48: bytes used in `det` below              */
    uint32_t flags;        /* 52: GUARDIAN_FRAME_*                       */
    uint32_t _pad0[2];     /* 56..63 */
    uint8_t  det[GUARDIAN_DETJSON_MAX];   /* 64 */
    uint8_t  jpeg[GUARDIAN_JPEG_MAX];
};

struct guardian_shm {
    uint32_t magic;         /*  0 */
    uint32_t version;       /*  4 */
    uint32_t active;        /*  8: index of the newest completed slot     */
    int32_t  target_fps;    /* 12: control, C -> detector                 */
    int32_t  target_width;  /* 16: control, C -> detector                 */
    int32_t  ctrl_seq;      /* 20: bumped when the control block changes  */
    int32_t  vision_pid;    /* 24 */
    int32_t  _pad0;         /* 28 */
    double   vision_start_ts; /* 32 */
    uint64_t frames_total;  /* 40 */
    /* CLOCK_MONOTONIC of the last frame that came from a real capture
     * source. The detector keeps publishing a "no signal" placeholder so the
     * dashboard and the MJPEG stream stay alive when the capture host goes
     * away, but the watchdog (part 4-3) must measure staleness against real
     * input -- otherwise the placeholder would mask a disconnected camera. */
    double   last_real_ts;  /* 48 */
    /* Detector network input edge in pixels (300 = the model's native size).
     * This dominates inference cost far more than the capture resolution, so
     * the thermal governor throttles it as well. */
    int32_t  target_net_input; /* 56 */
    int32_t  _pad2;         /* 60..63 -- header is exactly 64 bytes       */
    struct guardian_slot slot[GUARDIAN_NSLOTS];
};

/* A consistent snapshot handed back to callers. */
struct guardian_frame {
    double   ts_wall;
    double   ts_mono;
    int      persons;
    float    fps;
    int      width;
    int      height;
    float    infer_ms;
    uint32_t flags;
    uint32_t jpeg_len;
    uint8_t  jpeg[GUARDIAN_JPEG_MAX];
    char     det[GUARDIAN_DETJSON_MAX];
};

/* Map (and, if `create`, size) the segment. Returns false on failure. */
bool shmframe_open(bool create);
void shmframe_close(void);

/* Copy the newest complete frame. `out->jpeg` is filled only when
 * `want_jpeg` is set -- the telemetry paths do not need a 60 KB memcpy.
 * Returns false when no frame has ever been published. */
bool shmframe_read(struct guardian_frame *out, bool want_jpeg);

/* Cheap accessors that avoid copying the payload. */
bool  shmframe_last_ts(double *ts_mono, double *ts_wall);
int   shmframe_persons(void);
uint64_t shmframe_frames_total(void);

/* CLOCK_MONOTONIC of the newest frame that carried real capture input.
 * Returns false if no real frame has ever arrived. This -- not the frame
 * timestamp -- is what the software watchdog measures. */
bool  shmframe_last_real_ts(double *ts_mono);

/* Control channel (thermal governor / API commands). Any parameter may be
 * passed as 0 to leave it unchanged.
 *
 * Because 0 already means "unchanged", switching detection off needs its own
 * value: pass SHMFRAME_NET_OFF as net_input and the detector keeps relaying and
 * overlaying frames but runs no inference. The API exposes this to callers as
 * net_input = 0, which reads more naturally from outside. */
#define SHMFRAME_NET_OFF (-1)
void shmframe_set_target(int fps, int width, int net_input);
void shmframe_get_target(int *fps, int *width, int *net_input);

/* ------------------------------------------------------------ layout locks */
_Static_assert(sizeof(struct guardian_shm) ==
                   64u + GUARDIAN_NSLOTS * (64u + GUARDIAN_DETJSON_MAX + GUARDIAN_JPEG_MAX),
               "guardian_shm layout changed -- update vision/shmframe.py");
_Static_assert(offsetof(struct guardian_shm, slot) == 64, "header must be 64 bytes");
_Static_assert(offsetof(struct guardian_slot, det) == 64, "slot header must be 64 bytes");
_Static_assert(offsetof(struct guardian_slot, jpeg) == 64 + GUARDIAN_DETJSON_MAX,
               "jpeg offset changed");
_Static_assert(offsetof(struct guardian_shm, vision_start_ts) == 32, "header field moved");
_Static_assert(offsetof(struct guardian_shm, frames_total) == 40, "header field moved");

#endif /* GUARDIAN_SHMFRAME_H */
