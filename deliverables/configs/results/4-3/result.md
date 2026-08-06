# Experiment 4-3 -- software watchdog

Run: 2026-07-29T10:10:47Z. `watchdog_stale_sec = 30`.

The capture link was cut by dropping inbound TCP 9000 on the board and tearing
down the established connection, which models a camera or cable failure more
honestly than stopping the sender: the laptop keeps retrying throughout, and
recovers on its own the moment the rule is lifted.

| | |
|---|---|
| detector PID before | 3339 |
| detector PID after | 3745 |
| tampering email | 13:39:54.560 mailer: "[Guardian 402170516] CAMERA TAMPERING suspected" delivered in 2.3s |
| main daemon during the fault | active |

Recovery is measured by the detector's PID changing, not by systemd's
`NRestarts`. The watchdog recovers by asking systemd for a `restart` over
polkit, and `NRestarts` counts only the restarts systemd itself initiates
through `Restart=` after a failure -- it stays at 0 through a completely
successful watchdog recovery, which makes it a misleading thing to report.

The watchdog measures the age of the last frame that carried **real** capture
input, not the age of the last published frame. That distinction matters: the
detector keeps emitting a placeholder at 1 Hz so the dashboard does not freeze,
and a naive freshness check would happily watch those placeholders forever and
never fire.

Recovery is a restart of `guardian-vision` alone. An earlier revision of the
unit files would have taken the entire daemon down with it -- `guardian.service`
had `Requires=guardian-ingest.socket`, and that socket is `PartOf=` the
detector, so restarting the detector stopped the web server, MQTT, mail and the
black box with no path back. That is fixed, and the table above records the
daemon staying `active` while its detector was restarted underneath it.

Transcript: `cmd.log`.
