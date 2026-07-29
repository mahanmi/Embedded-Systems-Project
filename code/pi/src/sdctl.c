#include "sdctl.h"

#include "log.h"

#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>

static bool call_systemd(const char *method, const char *unit)
{
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        LOGE("sdctl: cannot open system bus: %s", strerror(-r));
        return false;
    }

    if (unit) {
        r = sd_bus_call_method(bus,
                               "org.freedesktop.systemd1",
                               "/org/freedesktop/systemd1",
                               "org.freedesktop.systemd1.Manager",
                               method, &err, &reply, "ss", unit, "replace");
    } else {
        /* logind's Reboot(interactive=false) */
        r = sd_bus_call_method(bus,
                               "org.freedesktop.login1",
                               "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager",
                               method, &err, &reply, "b", 0);
    }

    if (r < 0) {
        LOGE("sdctl: %s(%s) failed: %s", method, unit ? unit : "-",
             err.message ? err.message : strerror(-r));
    } else {
        LOGI("sdctl: %s(%s) accepted", method, unit ? unit : "-");
    }

    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return r >= 0;
}

bool sdctl_restart_unit(const char *unit)
{
    return call_systemd("RestartUnit", unit);
}

bool sdctl_reboot(void)
{
    return call_systemd("Reboot", NULL);
}

void sdctl_notify_ready(const char *status)
{
    char buf[256];
    snprintf(buf, sizeof buf, "READY=1\nSTATUS=%s", status ? status : "running");
    sd_notify(0, buf);
}

void sdctl_notify_status(const char *status)
{
    char buf[256];
    snprintf(buf, sizeof buf, "STATUS=%s", status ? status : "");
    sd_notify(0, buf);
}

void sdctl_notify_stopping(void)
{
    sd_notify(0, "STOPPING=1\nSTATUS=shutting down");
}
