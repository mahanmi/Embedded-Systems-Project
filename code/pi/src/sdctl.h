/* =========================================================================
 *  systemd control over sd-bus
 *
 *  The API's `reboot` command and the software watchdog's service restart
 *  both need privileged actions from a daemon that deliberately runs as an
 *  unprivileged user. Two ways to bridge that:
 *
 *    (a) a NOPASSWD sudoers entry and fork/exec of `systemctl`
 *    (b) a D-Bus call to org.freedesktop.systemd1, authorised by a polkit
 *        rule scoped to exactly the two units we own
 *
 *  (b) is used here. It is narrower -- polkit can allow "restart
 *  guardian-vision.service" without allowing "run anything as root" -- and it
 *  avoids spawning a shell from a network-facing daemon. It also keeps the
 *  action in C rather than delegating to a Linux command line.
 * ========================================================================= */
#ifndef GUARDIAN_SDCTL_H
#define GUARDIAN_SDCTL_H

#include <stdbool.h>

/* Restart a unit, e.g. "guardian-vision.service". */
bool sdctl_restart_unit(const char *unit);

/* Ask logind to reboot the board. */
bool sdctl_reboot(void);

/* Notify systemd that the daemon finished starting (Type=notify). */
void sdctl_notify_ready(const char *status);
void sdctl_notify_status(const char *status);
void sdctl_notify_stopping(void);

#endif /* GUARDIAN_SDCTL_H */
