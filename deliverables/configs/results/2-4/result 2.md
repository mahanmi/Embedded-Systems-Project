# Experiment 2-4 -- network lost mid-stream

Run: 2026-08-06T20:56:52Z. 300s sampled every 2 s by a logger running **on the
board**, so the outage itself is inside the record.

| | samples |
|---|---|
| total | 102 |
| carrier down | 14 |
| HTTP 200 overall | 102 |
| HTTP 200 **while the cable was out** | 14 |
| `mqtt_connected=false` | 8 |
| reconnected after the cable returned | 80 |

The measurement had to be planted before the fault: a sampler driven over SSH
would have died with the link it was watching, losing precisely the two minutes
of interest.

**What the board does when the network goes.** The HTTPS server keeps answering
on loopback and the detector keeps consuming frames, so the failure is confined
to what genuinely needs the network. The MQTT client notices, reports
`mqtt_connected=false` in telemetry, and retries with backoff rather than
spinning. Nothing is restarted and nothing crashes.

**Recovery is unattended.** When the carrier returns, the MQTT client reconnects
and republishes a retained `online` status; the capture host's own retry loop
re-establishes the MJPEG feed. No command is issued at either end.

Per-sample data: `network.csv`; board log across the window: `cmd.log`.
