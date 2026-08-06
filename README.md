# Smart Guardian System

A self-contained security appliance for a Raspberry Pi 3B+: it watches a camera
feed, detects people and vehicles, serves a live dashboard over TLS, publishes
telemetry to MQTT, sends email and Telegram alerts, records everything to a
bounded SQLite black box, restarts its own image pipeline when frames go stale,
and throttles itself when the SoC gets hot.

The constraint that shapes the whole design: **every piece of application logic
is in C** — around 6,000 lines across 15 modules — and Python is confined to the
single thing it is there for, OpenCV inference. The detector makes no decisions
about mail, MQTT, storage or the API; it hands annotated frames to the C daemon
through shared memory and stops there.

Built as an Embedded Systems final project. The full report is
[`Final_Proj_Embedded.pdf`](Final_Proj_Embedded.pdf); the experiment runbook is
[`deliverables/MANUAL_EXPERIMENTS.md`](deliverables/MANUAL_EXPERIMENTS.md).

![The dashboard](deliverables/screenshots/1-6_html_page.png)

---

## Architecture

```mermaid
flowchart LR
    subgraph sources["Frame source (pick one)"]
        DVR["Hikvision DVR<br/>RTSP · H.264 sub-stream"]
        CAM["Laptop webcam<br/>MJPEG over TCP"]
    end

    subgraph pi["Raspberry Pi 3B+"]
        SOCK["guardian-ingest.socket<br/>systemd owns :9000"]
        VIS["guardian_vision.py<br/>MobileNet-SSD · ROI crop · CLAHE"]
        SHM[("/dev/shm/guardian_frame<br/>seqlock double buffer")]

        subgraph daemon["guardian — the C daemon"]
            HTTPD["httpd · TLS<br/>:443 · :80 → 301"]
            API["REST /api/v1/*"]
            EVENTS["events<br/>telemetry · detect · supervisor"]
            STORE["store<br/>SQLite ring"]
            MQTT["mqttc"]
            MAIL["mailer"]
            TG["telegram"]
        end

        FAPI["guardian_api.py<br/>FastAPI · Swagger only"]
    end

    subgraph out["Off-board"]
        BROWSER["Browser dashboard"]
        BROKER["Mosquitto broker<br/>on the laptop"]
        SMTP["Gmail SMTP"]
        BOT["Telegram Bot API<br/>via SOCKS5"]
    end

    DVR -->|board pulls| VIS
    CAM --> SOCK --> VIS
    VIS -->|writes| SHM
    SHM -->|many readers| HTTPD
    SHM --> EVENTS
    EVENTS --> STORE & MQTT & MAIL & TG
    HTTPD --- API
    API -->|"FPS / resolution<br/>written back"| SHM
    HTTPD --> BROWSER
    FAPI -->|"127.0.0.1:8081"| HTTPD
    MQTT --> BROKER
    MAIL --> SMTP
    TG --> BOT
```

The systemd chain enforces the ordering, so nothing waits on a `sleep`:

```
guardian-ingest.socket → guardian-vision.service → guardian.service → guardian-api.service
```

`guardian.service` is `Type=notify` and calls `sd_notify(READY=1)` only once the
TLS listener, the database, the mail worker and the worker threads are all up —
which is what makes `After=guardian.service` in the API unit mean "after it can
actually serve" rather than "after fork".

---

## What's interesting here

### The shared-memory transport

[`src/shmframe.h`](code/pi/src/shmframe.h) and
[`vision/shmframe.py`](code/pi/vision/shmframe.py) implement a seqlock over a
double buffer: exactly one writer (the detector), any number of readers (HTTP
stream handlers, the detection consumer, the watchdog). A reader that observes a
torn slot retries; **a slow HTTP client can never stall the detector**, which a
socket or a pipe would allow.

The binary layout is mirrored byte-for-byte between the C struct and the Python
`struct` format string, and `_Static_assert`s at the bottom of the header make
any divergence a compile error rather than a runtime mystery.

It also doubles as the control channel — the C daemon writes target FPS and
resolution into the header and the detector picks them up. That is how the
thermal governor throttles the pipeline without an IPC round trip.

### A systemd dependency bug worth reading about

`guardian.service` originally declared `Requires=guardian-vision.service`. That
looked right and was wrong: systemd propagates a `Requires=` dependency's restart
to its dependents, so **every time the software watchdog restarted the detector,
it also tore down the web server, the MQTT client, the mailer and every open
MJPEG stream** — the exact opposite of what a watchdog is for.

The fix is `Wants=` plus `After=`: keep the pull-in and the ordering, drop the
lifetime coupling. A second instance of the same mistake arrived by a longer path
through `guardian-ingest.socket` (`PartOf=` the detector), and is documented too.
The reasoning is written into
[`systemd/guardian.service`](code/pi/systemd/guardian.service) rather than lost
to a commit message.

A side benefit: the dashboard now comes up and honestly reports "no signal" when
the detector cannot start, instead of the whole system disappearing.

### Decisions measured, not guessed

| Decision | Why | Evidence |
|---|---|---|
| Detect on a **square ROI crop**, not the whole frame | MobileNet-SSD takes square input, so a 16:9 frame stretches people 1.78× and it stops recognising them | 0/12 detections whole-frame vs **12/12** (mean conf 0.806) cropped |
| **CLAHE** contrast equalisation before inference | Night IR is far flatter than the daylight photos the model was trained on — a parked pickup came back as "chair 0.59" | vehicles/frame 0.00 → **1.01** over 89 night frames |
| CLAHE `clip=5 tile=4`, not the higher-recall `clip=8` | clip=8 scored 86% detection but **12.3%** false positives across 81 clean frames vs 0.0% | 271 labelled frames, labelled by background subtraction so the model wasn't grading its own homework |
| `vehicle_hold_frames = 8`, not 3 | Vehicles score barely above threshold, so a stationary car blinks in and out | counted in 13/20 samples at hold=3, **19/20** at hold=8 |
| `cv_threads = 2`, though 4 benchmarks faster | The extra sustained load crossed the thermal governor's setpoint, which then cut the stream to 2 fps | 1088 ms vs 1551 ms in isolation — and worse in practice |

Every one of these is annotated in place in
[`config/guardian.conf`](code/pi/config/guardian.conf) with the numbers behind
it, so the next person to touch a value can see what it costs.

### Credentials

[`.gitignore`](.gitignore) was written **before** `git init`, deliberately — once
a secret is committed it lives in the history whether or not a later commit
removes it.

- Every credential lives in `/etc/guardian/secrets.env` (`root:guardian`, `0640`),
  injected by systemd's `EnvironmentFile=`. Nothing is compiled in.
- The daemon **refuses to start** when a required secret is missing rather than
  falling back to a default.
- Putting a password-shaped key into `guardian.conf` is a hard startup error, and
  [`scripts/check_secrets.sh`](code/pi/scripts/check_secrets.sh) greps for exactly
  that mistake.
- The RTSP URL and the Telegram Bot API URL each embed their credential, which
  makes the *URL itself* a secret. Both are built at call time and logged
  redacted (`rtsp://admin:***@host/...`).

### Least privilege

- The daemon runs as `guardian`, never root. Ports 80 and 443 are reached with a
  single ambient `CAP_NET_BIND_SERVICE`, with `CapabilityBoundingSet` pinned to
  the same one.
- All three units carry `ProtectSystem=strict`, `ProtectHome`, `NoNewPrivileges`,
  `PrivateTmp`, `RestrictSUIDSGID`, `RestrictRealtime`, `LockPersonality`, and an
  explicit `ReadWritePaths=` allowlist.
- `MemoryMax` per unit (220M daemon / 420M detector / 160M API) so an OpenCV leak
  cannot take a 1 GB board down with it.
- The Mosquitto ACL ([`guardian.acl`](code/mac/mosquitto/guardian.acl)) scopes the
  board's credential to its own five topics, write-only. **The board subscribes to
  nothing** — it takes commands over the authenticated REST API only, never over
  MQTT.

### Honest measurement

The board is undervolted and clamped to 600 MHz — 43% of its 1.4 GHz rating.
Rather than quietly reporting the numbers, the affected figures are flagged
provisional at the point of use, with what would change once the supply is
replaced. See [Known caveats](#known-caveats).

In the same spirit, [`exp_latency.sh`](code/tests/exp_latency.sh) refuses to
trust its own clock correction past 60 s of offset: a Pi has no battery-backed
RTC, so a board that never reaches NTP keeps whatever stale time it booted with,
and that is not jitter to correct for but a broken clock to report.

---

## Hardware and stack

| | |
|---|---|
| Board | Raspberry Pi 3B+ — 4× Cortex-A53, 1 GB RAM, no NPU |
| Camera | Hikvision DS-7204HUHI-K1/P DVR (RTSP), or a laptop webcam over MJPEG |
| Off-board | macOS laptop running the Mosquitto broker and, optionally, the capture streamer |
| Detector | OpenCV DNN, MobileNet-SSD (Caffe, VOC) — class 15 = person, plus car/bus/motorbike from the same forward pass |
| C libraries | libmicrohttpd + GnuTLS, libmosquitto, json-c, sqlite3, libcurl, libsystemd |
| Python | OpenCV, NumPy, FastAPI, uvicorn |

Why MobileNet-SSD: on a Pi 3B+ with no accelerator it is the only readily
available detector that clears one frame per second at usable accuracy.
Experiment [3-3](code/tests/results/3-3/result.md) characterises the
resolution/FPS/accuracy trade-off.

---

## Repository layout

```
code/pi/                 everything that runs on the board
  src/                   the C daemon — 15 modules
    main.c               startup ordering, signal handling, teardown
    httpd.c  api.c       libmicrohttpd + TLS, and the REST surface
    events.c             telemetry, detection and supervisor threads
    shmframe.c/.h        the seqlock frame transport
    mqttc.c              MQTT client with broker-address failover
    mailer.c telegram.c  the two alert channels, each on its own thread
    store.c              SQLite black box
    sysinfo.c            /proc and /sys readers
    sdctl.c              sd_notify and logind/polkit calls
  vision/                guardian_vision.py + the Python side of shmframe
  api/                   guardian_api.py — FastAPI, Swagger only
  systemd/               the four units, with the ordering rationale in comments
  config/                guardian.conf, secrets.env.example, udev rules
  scripts/               bootstrap, install, fetch_model, gen_cert, harden_ssh
  www/                   the dashboard

code/mac/                laptop side
  stream_dvr.sh          DVR → laptop → board video link
  stream_webcam.sh       laptop webcam → board
  mqtt_tunnel.sh         SSH port-forward fallback for the broker
  subscribe_all.sh       watch every topic with local receive timestamps
  mosquitto/             broker config and the per-topic ACL

code/tests/              one script per experiment group, plus results/
deliverables/            report sources, screenshots, and the runbook
```

---

## Quick start

### On the board

```bash
sudo bash code/pi/scripts/bootstrap.sh
```

Installs the toolchain and dependencies, creates the `guardian` user, and
prepares `/opt/guardian`, `/etc/guardian` and `/var/lib/guardian`.

```bash
sudo bash code/pi/scripts/fetch_model.sh
```

Fetches the 23 MB Caffe model. It is deliberately not committed — it is
reproducible from this script.

```bash
sudo bash code/pi/scripts/install.sh
```

Builds the daemon natively (`make -j$(nproc)`; four A53 cores beat setting up a
cross toolchain for a project this size), installs binaries and units, and
generates a random API token. It **never clobbers a live `guardian.conf`** — a
changed config lands as `guardian.conf.new` alongside it.

```bash
sudo HOST_IP=<board lan ip> PUBLIC_NAME=<ddns name> \
     /opt/guardian/bin/gen_cert.sh
sudo nano /etc/guardian/secrets.env       # SMTP, MQTT, camera, Telegram
sudo bash code/pi/scripts/check_secrets.sh
```

```bash
sudo systemctl enable --now guardian-ingest.socket guardian-vision guardian guardian-api
```

Then browse to `https://<board>/` for the dashboard, or `https://<board>:8443/docs`
for Swagger.

### On the laptop

Start the broker and the capture feed:

```bash
/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/mosquitto.conf -d
cd code/mac && ./stream_dvr.sh          # or ./stream_webcam.sh
```

> **Neither of these is a managed service.** Both stop when you log out or
> reboot, and nothing brings them back. If the dashboard looks dead or the MQTT
> panel says offline, check these first — the board itself is almost certainly
> fine.
>
> ```bash
> pgrep -f 'stream_dvr|stream_webcam' >/dev/null && echo "streamer UP" || echo "streamer DOWN"
> ```

---

## Configuration

[`code/pi/config/guardian.conf`](code/pi/config/guardian.conf) holds non-secret
settings only, and is heavily annotated — most values carry the measurement that
chose them.

**Two frame sources**, selected by `camera_source`:

- `socket` — a capture host pushes MJPEG at `guardian-ingest.socket`. systemd
  owns the port, so it is bound the instant `sockets.target` is reached, long
  before OpenCV has finished loading a 23 MB model. No connection-refused race.
- `rtsp` — the board pulls H.264 straight from the DVR, and no laptop is involved
  at all. Always a sub-stream: the main streams are H.265 up to 2560×1440 and the
  Pi 3B+ has no H.265 hardware decoder.

Tunables worth knowing:

| Key | Default | Notes |
|---|---|---|
| `target_fps` | 15 | Matched to the capture host's wire rate; the thermal governor steps it down on its own |
| `detect_roi` / `detect_roi_align` | `auto` / `right` | The square crop — see the table above |
| `enhance_contrast` | `true` | CLAHE, with `clahe_clip_limit` / `clahe_tile_grid` |
| `cv_threads` | 2 | Judge on sustained temperature, not inference latency |
| `detect_vehicles` | `true` | Counted and drawn **only** — never reaches the mailer, the alarm or the black box, because a parked car is permanent |
| `thermal_high_c` / `thermal_low_c` | 75 / 68 | The 7 °C gap is hysteresis, to stop the level flapping |
| `watchdog_stale_sec` | 30 | Frame age before the detector is restarted |
| `mail_debounce_sec` | 30 | At most one detection email per 30 s |

Secrets go in `/etc/guardian/secrets.env` — see
[`secrets.env.example`](code/pi/config/secrets.env.example) for the five keys and
what each is for.

---

## REST API

Served by the C daemon on `:443`. Swagger UI is on `:8443/docs`, provided by a
FastAPI gateway that forwards to the daemon's loopback listener and holds no
logic of its own.

| Method | Endpoint | Purpose |
|---|---|---|
| `GET` | `/api/v1/persons` | Current person (and vehicle) count |
| `GET` | `/api/v1/telemetry` | CPU temperature, memory, load, FPS |
| `GET` | `/api/v1/history?limit=N` | Recent detections from the black box |
| `GET` | `/api/v1/stats` | Lifetime counters (survive the ring buffer) |
| `GET` | `/api/v1/guard` | Anti-theft mode state |
| `POST` | `/api/v1/guard` | Arm or disarm anti-theft mode |
| `GET` | `/api/v1/health` | Per-component health |
| `GET` | `/api/v1/commands` | The command catalogue — discoverable, not hardcoded |
| `POST` | `/api/v1/command` | Execute a command |
| `GET` | `/api/v1/stream` | Live MJPEG stream |

Commands are a dispatch table rather than an if/else chain, so adding one is a
single entry and `/api/v1/commands` publishes the table — clients and the Swagger
layer discover what exists without a code change.

| Command | Privileged | |
|---|---|---|
| `ping` | | Liveness check; performs no action |
| `guard_on` / `guard_off` | ✓ | Arm / disarm anti-theft mode |
| `set_fps` | ✓ | Change the detector's target frame rate |
| `set_resolution` | ✓ | Change the capture width |
| `set_net_input` | ✓ | Change the network's input edge |
| `snapshot` | ✓ | Email the current annotated frame |
| `publish_telemetry` | ✓ | Force an immediate MQTT publish |
| `restart_vision` | ✓ | Restart the image-processing service |
| `reboot` | ✓ | Reboot the board (answers the client first, then goes down) |

Privileged commands require a bearer token from `GUARDIAN_API_TOKEN`.

---

## MQTT

Five topics, all under the student number:

| Topic | Contents |
|---|---|
| `telemetry/<id>/home` | Temperature, memory, load, FPS — every 5 s |
| `persons/<id>/home` | Person count on change |
| `alarm/<id>/home` | Guard-mode alarms |
| `status/<id>/home` | Online/offline, retained — this is the LWT topic |
| `event/<id>/home` | Watchdog and thermal events |

The board's client publishes a **retained Last Will** on `status/`, so a board
that dies without a clean DISCONNECT is reported by the broker itself.
Experiment [3-4](code/tests/results/3-4/result.md) demonstrates this by
SIGKILLing the daemon while the broker watches — killing the *broker* cannot
demonstrate an LWT, because then nothing is left to publish it.

The client also fails over between two broker addresses: the laptop's Bonjour
name, and `127.0.0.1` where an SSH port-forward
([`mqtt_tunnel.sh`](code/mac/mqtt_tunnel.sh)) presents the same broker on the
board's loopback. No config edit when the laptop moves networks.

---

## Alerting

Email and Telegram run as **parallel channels with independent debounce**, each
on its own worker thread — a stalled proxy connection can never hold up an email.
Both carry the same four events: detection, guard alarm, watchdog, thermal.

Telegram also **listens**. A second thread long-polls `getUpdates` and answers a
small command set (`/preview` returns a photo of the current frame, `/help`,
`/start`). Two deliberate constraints:

- **Long polling, not a webhook** — forced, not chosen. A webhook needs Telegram
  to open a connection *into* the board, and the only route this network has to
  the API is an outbound SOCKS5 proxy.
- **Every inbound command is read-only.** Arming the alarm, changing the frame
  rate and rebooting stay behind the REST bearer token, because a chat is
  authenticated by whoever is holding the phone. Commands are honoured only from
  the configured chat id; anyone else is refused before a frame is even read.

Telegram is allowed to fail at startup, like MQTT and unlike mail — a board that
refused to guard because a proxy was down would have the priorities backwards.

---

## Reliability

**Software watchdog** — a supervisor thread times frame freshness against
`watchdog_stale_sec`. Past the threshold it restarts `guardian-vision.service`
and sends a camera-tampering alert. A 45 s startup grace covers the Caffe model
load. Experiment [4-3](code/tests/results/4-3/result.md) cuts the capture link by
dropping inbound TCP 9000 — which models a cable failure more honestly than
stopping the sender, since the laptop keeps retrying throughout.

**Adaptive thermal governor** — steps `target_fps` and resolution down through
levels as the SoC heats, with hysteresis between `thermal_high_c` and
`thermal_low_c` so the level cannot flap. The setpoint was raised from 70 °C to
75 °C after the governor tripped 24 times in six hours once the board began
decoding H.264 — correct behaviour on a setpoint set too tight, but it made the
dashboard feel broken.

**Black box** — SQLite, bounded by an `AFTER INSERT` trigger that deletes
anything older than the newest `db_ring_size` rows, so the database cannot grow
without limit on a card with no log rotation. Lifetime counters live in a
separate `stats` table precisely so they survive the ring.

**Restart policy** — every unit is `Restart=always` with
`StartLimitIntervalSec=0`, so systemd never gives up and disables a unit.
Experiment [1-2](code/tests/results/1-2/result.md) `kill -9`s the daemon and
watches it come back.

---

## Experiments

19 experiments with committed data under
[`code/tests/results/`](code/tests/results/). Each directory has a `result.md`
with the write-up and the raw CSV/log alongside it.

| # | Experiment | Headline |
|---|---|---|
| [1-1](code/tests/results/1-1/result.md) | Boot time, unattended start | 1 min 4.8 s total; `guardian.service` 2.6 s of it |
| [1-2](code/tests/results/1-2/result.md) | `SIGKILL` the web server | Back in ~10 s, restart counter 0 → 1 |
| [1-4](code/tests/results/1-4/result.md) | HTTP → HTTPS redirect | 301, `Location` follows the request's own `Host` |
| [1-5](code/tests/results/1-5/result.md) | Self-signed certificate | SAN covers IP, `mahan`, `mahan.local`, `localhost` |
| [1-6](code/tests/results/1-6/result.md) | The served page | Live stream, count, 2 s telemetry refresh |
| [2-1](code/tests/results/2-1/result.md) | CPU temp in three states | 59.9 / 60.4 / 62.3 °C idle → stream → detect |
| [2-2](code/tests/results/2-2/result.md) | Memory under continuous streaming | +16 kB over 300 s; steady-state slope 0.014 kB/s |
| [2-3](code/tests/results/2-3/result.md) | 50 concurrent API requests | 9.0× slowdown, 50/50 succeed, zero refused |
| [2-4](code/tests/results/2-4/result.md) | Network lost mid-stream | HTTPS keeps serving on loopback; MQTT reconnects unattended |
| [3-1](code/tests/results/3-1/result.md) | Accuracy by lighting | 20/20 daylight, artificial and backlight; **6/20 low light** |
| [3-2](code/tests/results/3-2/) | Photo/screen spoofing | Raw data only — see [Known caveats](#known-caveats) |
| [3-3](code/tests/results/3-3/result.md) | Input resolution sweep | 300/224/160 px → 1444/859/466 ms inference |
| [3-4](code/tests/results/3-4/result.md) | LWT and a broker outage | Retained will fires on `status/`; client recovers |
| [3-5](code/tests/results/3-5/result.md) | Detection → MQTT latency | 1517 ms mean, sd 33 ms, over 10 rising edges |
| [3-6](code/tests/results/3-6/result.md) | Unauthorised MQTT | 4/4 refused; authorised client stayed up as a control |
| [3-7](code/tests/results/3-7/result.md) | Unauthorised SSH | 3/3 refused; no password prompt is ever offered |
| [4-2](code/tests/results/4-2/result.md) | The black box | 607 rows against a 1000-row ring; stats survive |
| [4-3](code/tests/results/4-3/result.md) | Software watchdog | Detector restarted, tampering email in 2.3 s |
| [4-4](code/tests/results/4-4/result.md) | Adaptive thermal management | Level 3 engaged, `target_fps` cut to 2 |

Experiments 1-3 (cold boot) and 4-1 (guard mode) are evidenced by screen
recording rather than by a data file; the recordings are submitted alongside the
repo rather than committed to it.

Most experiments run from the laptop as a single script —
`code/tests/exp_web.sh`, `exp_perf.sh`, `exp_resolution.sh`, and so on. Seven
need a physical action (pulling a cable, holding a photo to the lens, walking
into frame); [`MANUAL_EXPERIMENTS.md`](deliverables/MANUAL_EXPERIMENTS.md) walks
through every one.

---

## Known caveats

**The board is undervolted.** It runs clamped to 600 MHz — 43% of its 1.4 GHz
rating — and replacing the cable did not fix it, so it is the power supply. Every
benchmark here was taken at that clock. The figures most affected are flagged in
place:

- The `cv_threads` choice, weighed on a latency/temperature trade-off that will
  not be the same one at full clock.
- The 75 °C thermal setpoint, chosen against a board that idled at 52 °C and
  reached only 58–61 °C with all four cores pinned.
- The RTSP decode cost (49% of one core) that currently rules `rtsp` mode too
  expensive — that is 49% of a core running at 43% speed.

**The board's clock is unsynced** and roughly 62 days behind. Experiment 3-5's
latency figures are corrected against that offset and are only as trustworthy as
it is; the script now says so in its own output. Check `timedatectl status` before
trusting any board-side timestamp.

**The deployed config drifts from this repo, deliberately.** `guardian.conf` here
says `camera_source = rtsp` — the target state — while the board runs `socket`.
Check the board before diagnosing a live fault:

```bash
ssh <board> 'sudo grep ^camera_source /etc/guardian/guardian.conf'
```

**Experiment 3-2 is incomplete.** It shipped `spoof.csv` with no write-up, and the
raw data needs a second look before it can be reported: the `empty` control
registered people in 23 of 33 samples, while both `real` and `screen` came back
10/10.

---

## Report

- [`Final_Proj_Embedded.pdf`](Final_Proj_Embedded.pdf) — the full report
- [`deliverables/Tex/`](deliverables/Tex/) — its LaTeX sources
- [`deliverables/MANUAL_EXPERIMENTS.md`](deliverables/MANUAL_EXPERIMENTS.md) — the
  experiment runbook
- [`deliverables/screenshots/`](deliverables/screenshots/) — visual evidence

## License

Coursework, published for reference. No license is granted for reuse — if you
want one here, add it explicitly.
