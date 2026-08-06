# Experiment 1-1 -- boot time and unattended start

Run: 2026-07-27T08:59:35Z. Boot timestamp moved 2026-07-27 03:47:23 -> 2026-07-27 12:26:51, so this is a
genuine reboot rather than a reconnect.

```
Startup finished in 11.812s (kernel) + 53.022s (userspace) = 1min 4.835s 
graphical.target reached after 44.195s in userspace.
```

Our own units, from `systemd-analyze blame`:

```
 2.617s guardian.service
  115ms guardian-ingest.socket
```

`systemd-analyze blame` lists **initialisation time per unit, not a critical
path** -- the entries overlap heavily and do not sum to the total. The number
that explains when the system became usable is `critical-chain`, in
`cmd.log`.

The detector is the slowest of ours because it loads a 23 MB Caffe model before
it can serve a frame. That is start-up cost, not a stall, and it is why the
ingest socket is a separate unit: the port is bound at `sockets.target`, so the
capture host can connect during that window instead of being refused.

All four units came up with nothing typed on the board and the dashboard
answered HTTP 200, which is the unattended-start requirement. The full
per-unit table is in `cmd.log` -- screenshot it for the report.
