/* =========================================================================
 *  Telegram alerts -- the second notification channel.
 *
 *  Structurally a sibling of mailer.c rather than a part of it: same queue,
 *  same coalescing debounce, same health model, but its own worker thread and
 *  its own libcurl handle. That separation is deliberate and is the whole
 *  reason this is not simply another call inside the mail worker:
 *
 *    * api.telegram.org is only reachable from this network through a SOCKS5
 *      proxy, which is a consumer service and fails far more often than Gmail.
 *      Sharing a thread would let a stalled proxy connection hold up an email
 *      that would otherwise have gone out.
 *
 *    * /api/v1/health has to be able to say "mail up, telegram down". One
 *      combined verdict would hide whichever channel was still working.
 *
 *  The trade is a queue and a worker loop that echo mailer.c's. The debounce
 *  semantics are documented there and are not repeated here; the only thing
 *  worth restating is that this module coalesces detections the same way, so
 *  the photo that arrives is the worst observation of the interval rather than
 *  the first.
 *
 *  Transport is HTTPS to the Bot API: sendPhoto with the JPEG as a
 *  multipart/form-data part when there is a frame, sendMessage when there is
 *  not. The bot token is a credential that lives in the request PATH, so it is
 *  built into the URL at call time from the environment and never appears in
 *  a log line, an error message or this repository.
 *
 *  INBOUND COMMANDS
 *  ----------------
 *  A second thread long-polls getUpdates and answers a small read-only command
 *  set (/preview, /help, /start). Long polling rather than a webhook is forced
 *  by the deployment, not chosen: a webhook needs Telegram to open a connection
 *  TO the board, which is behind NAT with only an outbound SOCKS5 proxy to
 *  reach the API at all.
 *
 *  Nothing in the command set mutates state. Arming the alarm, changing the
 *  frame rate and rebooting stay behind the REST API's bearer token, because a
 *  chat is authenticated by whoever is holding the phone.
 *
 *  Authorisation is the load-bearing part: anyone can message a bot, so a
 *  command is honoured only when message.chat.id matches telegram_chat_id.
 * ========================================================================= */
#ifndef GUARDIAN_TELEGRAM_H
#define GUARDIAN_TELEGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The alert taxonomy is shared with the mail channel rather than duplicated:
 * every trigger notifies both, and two enums that had to be kept in step would
 * be a bug waiting to happen. */
#include "mailer.h"

/* Returns false when the channel is configured but the worker cannot start.
 * A disabled channel (no bot token, no chat id) is a success: the caller
 * treats Telegram as optional, unlike mail. */
bool telegram_start(void);
void telegram_stop(void);

/* True when the message goes out immediately, false when the debounce folded
 * it into the pending slot. Mirrors mailer_notify_detection(). */
bool telegram_notify_detection(int persons, double ts_wall, double cpu_temp_c,
                               const uint8_t *jpeg, size_t jpeg_len,
                               bool guard_mode);

/* Operational alerts. `caption` is the whole message -- Telegram has no
 * subject line, and captions are capped at 1024 characters, so callers pass a
 * compact summary rather than the multi-paragraph body the mailer sends. */
void telegram_notify_event(mail_kind_t kind, const char *caption,
                           const uint8_t *jpeg, size_t jpeg_len);

/* Same three-state model as struct mailer_health: never-attempted is its own
 * state and must not be reported as a failure. */
struct telegram_health {
    bool     enabled;               /* configured at all?                   */
    bool     attempted;
    bool     ok;
    double   last_attempt_wall;
    unsigned consecutive_failures;

    /* Inbound half. Reported separately because it fails separately: the
     * send path can be perfectly healthy while getUpdates is being refused. */
    bool     polling;               /* command poller thread running        */
    unsigned poll_failures;         /* consecutive getUpdates failures      */
    double   last_update_wall;      /* last command received; 0 if none     */
    uint64_t commands_handled;
};

void telegram_health(struct telegram_health *out);

#define TELEGRAM_FAIL_DEGRADE 3u

bool telegram_healthy(void);

#endif /* GUARDIAN_TELEGRAM_H */
