/* =========================================================================
 *  Email alerts (part 3-b) -- entirely in C, as the brief requires.
 *
 *  Transport is libcurl speaking SMTP over implicit TLS to Gmail; the MIME
 *  multipart/mixed body (text part + base64 JPEG attachment) is assembled by
 *  hand rather than pulled in from a mail library, so the whole path stays
 *  visible in this file.
 *
 *  DEBOUNCE (the brief: "at most one email every 30 seconds")
 *  ----------------------------------------------------------
 *  A naive rate limiter drops everything inside the window, which means the
 *  most interesting frame -- the one where a second person walked in -- is the
 *  one most likely to be thrown away. Instead this module *coalesces*:
 *
 *      trigger arrives
 *        |
 *        +-- window open  (now - last_send >= debounce) --> send immediately
 *        |
 *        +-- window closed --> overwrite the single pending slot with this
 *                              (newer, higher-person-count) snapshot and let
 *                              the worker fire it the instant the window opens
 *
 *  So the guarantee is "never more than one message per debounce interval",
 *  while the message that does go out always carries the *worst* observation
 *  from that interval rather than the first. Suppressed-but-superseded events
 *  are counted and reported via /api/v1/stats.
 *
 *  Guard mode (part 4-1) swaps the 30 s interval for a much shorter floor,
 *  which is the anti-flood minimum rather than a batching window.
 * ========================================================================= */
#ifndef GUARDIAN_MAILER_H
#define GUARDIAN_MAILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MAIL_DETECTION,      /* person(s) detected                    */
    MAIL_ALARM,          /* guard mode: intrusion                 */
    MAIL_WATCHDOG,       /* part 4-3: camera tampering            */
    MAIL_THERMAL,        /* part 4-4: throttling engaged/released */
} mail_kind_t;

bool mailer_start(void);
void mailer_stop(void);

/* Queue a detection alert. Subject to the coalescing debounce above.
 * `jpeg` may be NULL; when present it is attached as detection.jpg.
 *
 * Returns true when the debounce window was open and the message goes out
 * immediately, false when it was coalesced into the pending slot instead. The
 * black box records this so the history distinguishes "an email was sent for
 * this event" from "this event was folded into a later email". */
bool mailer_notify_detection(int persons, double ts_wall, double cpu_temp_c,
                             const uint8_t *jpeg, size_t jpeg_len,
                             bool guard_mode);

/* Operational alerts (watchdog, thermal). These bypass the detection
 * debounce -- they are rare by construction -- but each kind has its own
 * minimum re-send interval so a flapping condition cannot spam the inbox. */
void mailer_notify_event(mail_kind_t kind, const char *subject,
                         const char *body, const uint8_t *jpeg, size_t jpeg_len);

/* True once at least one message has been delivered successfully. */
bool mailer_healthy(void);

#endif /* GUARDIAN_MAILER_H */
