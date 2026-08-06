# Experiment runbook — every test, 1-1 through 4-4, one screenshot each

Every experiment the brief asks for is listed below in order, 1-1 through 4-4.
Seven of them (1-1, 1-3, 2-4, 3-1, 3-2, 3-5, 4-1) need a physical action, a
camera pointed at something, or a video recording. The rest are a single
script you run from the laptop — no hands-on-hardware step, just a command and
a wait. Either way, every one below ends with an explicit screenshot to save
for the report, so you can work through this list one entry at a time and know
exactly what to run and what to capture.

Run each from the project root unless noted. The board is `mahan.local`,
student number `402170516`.

> **Address the board by name, not by IP.** It is a DHCP client and its address
> moves — it went `.26` → `.33` on 2026-08-02 — which breaks every hardcoded
> reference at once. `mahan.local` works on this LAN only; from off-LAN, set
> `PI_HOST` / `BOARD_HOST` / `GUARDIAN_HOST` explicitly.

> **⚠ The recorded timings in experiments 3-3 and 4-4 are provisional.** They were
> measured while the board was clamped to 600 MHz by undervoltage (43% of its
> 1.4 GHz rating) — see the note in `code/pi/config/guardian.conf`. Both need
> re-running once the power supply is replaced. Everything else here is unaffected.

Before starting any of them, make sure the capture feed is up:

```bash
cd code/mac && ./stream_webcam.sh
```

## Two things that must be running, and die on logout

Neither of these is a managed service, so **both stop when you log out or reboot
and nothing brings them back**. If the dashboard looks dead or the MQTT panel
says offline, check these first — the board itself is almost certainly fine.

| what | symptom when missing | restart |
|---|---|---|
| the capture streamer | dashboard shows "no signal"; after 30 s the watchdog starts emailing "camera tampering" | `cd code/mac && ./stream_dvr.sh` (DVR) or `./stream_webcam.sh` (laptop camera) |
| mosquitto broker | dashboard "MQTT broker: offline"; board retries every 25 s forever | `/opt/homebrew/opt/mosquitto/sbin/mosquitto -c /opt/homebrew/etc/mosquitto/mosquitto.conf -d` |

Quick check of both:

```bash
pgrep -f 'stream_dvr|stream_webcam' >/dev/null && echo "streamer UP" || echo "streamer DOWN"; pgrep -f 'sbin/mosquitto' >/dev/null && echo "broker UP" || echo "broker DOWN"
```

To make them survive reboots, the broker can be handed to launchd with
`brew services start mosquitto`. That installs a login agent, so it is a change
to your machine rather than to this project — do it deliberately, not by accident.

**Only ever run one copy of the streamer.** A second one competes for the DVR
channel and for the board's ingest socket, and the loser produces hundreds of
H.265 reference-frame errors that look like a broken camera. The script now
refuses to start a second copy and names the PID of the one already running.

---

## Two-site operation: the board away from the DVR

The DVR stays at the house. The board and the laptop move to the other site.
That leaves exactly one hop crossing the internet, and **which hop it is depends
on `camera_source`** — check the deployed value before debugging anything, since
`/etc/guardian/guardian.conf` has drifted from the repo copy before:

```bash
ssh mahan@$PI_HOST 'sudo grep -E "^camera_source" /etc/guardian/guardian.conf'
```

| `camera_source` | who pulls RTSP from the house | what needs the home address |
|---|---|---|
| `rtsp` | the board, directly | `camera_host` in `guardian.conf` |
| `socket` | the laptop's `stream_dvr.sh`, which re-pushes MJPEG to the board over the local LAN | `CAMERA_HOST` in the streamer's environment |

`rtsp` is the better mode once the board is away: it takes the laptop off the
critical path entirely, so the system keeps working when the laptop sleeps,
logs out or goes elsewhere — the failure the section above is all about.

Set the addresses with environment variables rather than editing files; every
script already reads them:

```bash
export PI_HOST=<new-site ddns name>      # code/tests/*, deliverables commands
export CAMERA_HOST=<home ddns name>      # code/mac/stream_dvr.sh, socket mode only
```

Use DDNS names, not literal addresses. Both sites have residential IPs that
rotate, and a hardcoded one fails silently at the worst moment.

Three things to know before relying on it:

* **Upload, not download, is the limit.** The house has to push the sub-stream
  out continuously. Measure both before moving anything, and aim for sustained
  upload of about twice the stream bitrate:
  ```bash
  ffprobe -v error -rtsp_transport tcp -show_entries format=bit_rate -of csv=p=0 \
      -i "rtsp://admin:$CAMERA_PASS@$CAMERA_HOST:554/Streaming/Channels/202"
  ```
* **The certificate has to carry the new name**, or every remote visit is a name
  mismatch on top of the self-signed warning:
  ```bash
  sudo HOST_IP=<new lan ip> PUBLIC_NAME=<new-site ddns name> /opt/guardian/bin/gen_cert.sh
  sudo systemctl restart guardian
  ```
* **Stalls look different over a WAN.** A link that dies mid-stream used to wedge
  the reader; `guardian_vision.py` now sets a 5 s FFmpeg socket deadline so it
  reconnects instead. The symptom to watch for is `frame_age_sec` climbing and
  never resetting:
  ```bash
  ssh mahan@$PI_HOST 'curl -sk https://127.0.0.1/api/v1/health' | grep -o '"frame_age_sec":[0-9.]*'
  ```

---

## 1-1 — boot time (`systemd-analyze blame`)

Nothing physical is strictly required; I can run this remotely on request. It is
listed here only because the brief asks for a **terminal screenshot**.

```bash
ssh mahan@mahan.local 'sudo systemctl reboot'
```

Wait ~60 s, then:

```bash
ssh mahan@mahan.local 'systemd-analyze; echo; systemd-analyze blame | head -25; echo; systemd-analyze critical-chain guardian.service'
```

Screenshot the terminal and save it as
`deliverables/screenshots/1-1_boot_time.png`. Expected: total boot time, then
the per-unit table with `guardian`, `guardian-vision` and `guardian-api`
visible.

> The detector dominates its own start-up because it loads a 23 MB Caffe model.
> That is worth one sentence in the report — it is start-up cost, not a stall.

---

## 1-2 — kill -9 the web server, watch it self-heal

Nothing physical required. This and 1-4/1-5/1-6 below all come from one
script:

```bash
cd code/tests && ./exp_web.sh
```

It runs 1-2, 1-4, 1-5 and 1-6 in one pass. For 1-2 specifically it sends
`SIGKILL` to the web server, times how long systemd takes to bring a new
process up, and writes the journal extract to `cmd.log`.

Screenshot the terminal segment covering the kill → restart → journal tail and
save it as `deliverables/screenshots/1-2_web_restart.png`.

---

## 1-4 — http → https redirect

Produced by the same `./exp_web.sh` run as 1-2 above — no separate command.

The report wants a browser screenshot here, not a terminal one: open
`http://mahan.local/` with devtools on the **Network** tab and photograph the
301 entry. Save it as `deliverables/screenshots/1-4_https_redirect.png`.

---

## 1-5 — the self-signed certificate

Also produced by the `./exp_web.sh` run. The script pulls the certificate
fields (CN, issuer, SANs) into `cmd.log`, but the report wants the padlock
warning itself: open `https://mahan.local/`, click through the self-signed
warning, and screenshot the certificate/padlock dialog. Save it as
`deliverables/screenshots/1-5_certificate.png`.

---

## 1-6 — the served HTML page

Also produced by the `./exp_web.sh` run — it confirms the student number and
the stream/persons/telemetry hooks are present in the markup. Open
`https://mahan.local/` and screenshot the page itself with the title bar
visible, save as `deliverables/screenshots/1-6_html_page.png`.

---

## 1-3 — cold power cycle, no keyboard

**Physical:** pull the board's power, wait ~10 s, plug it back in. Film the whole
thing on your phone, from the board being dark to the dashboard loading.

Point the camera so the recording shows you never touch a keyboard. On the laptop
have the browser open at `https://mahan.local/` and hit reload as it comes up —
the video should show the page going from unreachable to live on its own.

To timestamp the recovery for the report:

```bash
ssh mahan@mahan.local 'uptime -s; systemd-analyze'
```

Screenshot that terminal output and save it as
`deliverables/screenshots/1-3_cold_boot_timestamp.png` — the report wants a
still alongside the video.

Save the video as `deliverables/videos/1-3_cold_boot.mp4`.

---

## 2-1 — CPU temperature in three states

Nothing physical required. Runs on the board over ~15 minutes (idle,
stream-only, stream+detect — 5 minutes each):

```bash
cd code/tests && ./exp_perf.sh 2-1
```

While it's sampling "idle" the laptop's capture streamer will print
`Connection refused` and retry — expected, not a fault, since the detector is
briefly stopped for that state. It reconnects on its own once state (b)
starts the detector again.

Screenshot the terminal's final three-state table and save it as
`deliverables/screenshots/2-1_thermal_states.png`.

---

## 2-2 — memory of the C daemon under continuous streaming

Nothing physical required. ~5 minutes:

```bash
cd code/tests && ./exp_perf.sh 2-2
```

Samples RSS every 5 s while streaming and reports a leak/no-leak verdict from
the steady-state slope.

Screenshot the terminal's summary table and save it as
`deliverables/screenshots/2-2_memory_growth.png`.

---

## 2-3 — 50 concurrent requests to /api/v1/telemetry

Nothing physical required.

```bash
cd code/tests && ./exp_perf.sh 2-3
```

Fires 50 requests at once from the board and reports latency (min/mean/max,
p95) and temperature before and after.

Screenshot the terminal summary and save it as
`deliverables/screenshots/2-3_api_load.png`.

---

## 2-4 — network drop mid-stream

**Physical:** unplug the board's Ethernet cable while the stream is running, wait
**2 minutes**, plug it back in.

Start this before you unplug — it records the whole window:

```bash
cd code/tests && ./exp_network.sh
```

If you would rather do it by hand, the evidence to capture afterwards is:

```bash
ssh mahan@mahan.local 'sudo journalctl -u guardian --since "-5 minutes" --no-pager | grep -iE "mqtt|httpd|reconnect"'
```

Screenshot that terminal output and save it as
`deliverables/screenshots/2-4_network_drop.png`.

Expected: MQTT reports disconnected and retries with backoff; the web server and
detector keep running locally; when the cable returns the client reconnects with
no intervention. The capture host reconnects on its own too — that is the retry
loop in `stream_webcam.sh`.

---

## 3-1 — accuracy in four lighting conditions

**Physical:** stand in frame under each condition below for ~60 s while the script
samples. Four runs, one per condition.

```bash
cd code/tests
./exp_lighting.sh --condition daylight
./exp_lighting.sh --condition artificial
./exp_lighting.sh --condition lowlight
./exp_lighting.sh --condition backlight
```

- **daylight** — room lit by the window, no lamps
- **artificial** — curtains shut, ceiling light on
- **lowlight** — curtains shut, lights off, screen glow only
- **backlight** — you standing directly in front of the window or a bright lamp,
  so the camera meters for the background and you fall into silhouette

Save one still per condition into `deliverables/screenshots/` (grab them from
`https://mahan.local/` — the overlay already carries the student number, date
and live clock, which is what the brief wants visible):

- `3-1_daylight.png`
- `3-1_artificial.png`
- `3-1_lowlight.png`
- `3-1_backlight.png`

> Backlight is the one that will fail. Say so in the report and explain why: the
> sensor exposes for the bright background, your face and torso lose contrast,
> and an SSD trained on well-exposed people has nothing left to key on.

---

## 3-2 — can a printed photo fool it?

**Physical:** hold a photo of a person in front of the camera — printed on paper
*and* displayed on your phone screen, they behave differently.

```bash
cd code/tests && ./exp_spoof.sh
```

Capture a still of each attempt being detected (or not) and save them as:

- `deliverables/screenshots/3-2_spoof_printed.png` — printed photo
- `deliverables/screenshots/3-2_spoof_screen.png` — photo on the phone screen

> Expect it **to be fooled**. MobileNet-SSD is a single-frame appearance detector
> with no liveness cue whatsoever — it has no notion of depth, texture, motion
> parallax or pulse, so a sharp photo is simply a person to it. The report should
> say this plainly and then propose remedies: depth or IR imaging, blink/motion
> liveness over consecutive frames, texture analysis for print/screen artefacts
> (moiré, specular reflection), or requiring parallax across a moving camera.
> Do not claim a fix you have not implemented — naming the right defence and
> explaining the trade-off is the answer here.

---

## 3-3 — input resolution sweep

**Physical:** stand in frame for the whole sweep so the detection-rate columns
mean something (`--person`).

```bash
cd code/tests && ./exp_resolution.sh --person
```

Sweeps the detector's input edge across 300 / 224 / 160 px, 5 minutes per
level (~15 minutes total). See the ⚠ note at the top of this file — this
experiment's numbers are provisional until the power supply is replaced.

Screenshot the terminal's final table (FPS / temperature / memory /
detection-rate per level) and save it as
`deliverables/screenshots/3-3_resolution_sweep.png`.

---

## 3-4 — LWT and a broker outage

Nothing physical required, but it takes ~4 minutes (a keepalive wait plus a
3-minute broker outage):

```bash
cd code/tests && ./exp_features.sh 3-4
```

Part 1 sends `SIGKILL` to the board's client so the broker fires the Last
Will on `status/402170516/home`; part 2 stops the broker for 3 minutes and
confirms the web server and detector keep working without it, then restarts
the broker and confirms the board reconnects on its own.

Screenshot the terminal showing the captured will message and the
reconnection at the end, save as
`deliverables/screenshots/3-4_lwt_outage.png`.

---

## 3-5 — detection → MQTT latency, 10 samples

**Physical:** walk into frame, wait for the `PASS` line, step out. Repeat 10×.

```bash
cd code/tests && ./exp_latency.sh
```

It measures the laptop/board clock offset first and subtracts it — the two ends
keep independent clocks and, at a few hundred ms, an uncorrected NTP disagreement
would be bigger than the thing being measured. Only 0→N transitions are timed, so
"still in frame" messages do not count as fresh arrivals.

Reports mean, standard deviation, min and max, and writes `latency.csv`.
Screenshot the terminal's summary line and save it as
`deliverables/screenshots/3-5_latency.png`.

---

## 3-6 — unauthorised MQTT access

Nothing physical required. This and 3-7 below come from one script:

```bash
cd code/tests && ./exp_security.sh
```

Tries an anonymous subscribe, a wrong password, and an unknown user against
the broker — all three must be refused. If `code/mac/.secrets/mqtt.env` has
viewer credentials, it also verifies the ACL silently drops a publish from
the read-only account.

Screenshot the terminal output for 3-6 and save it as
`deliverables/screenshots/3-6_mqtt_unauthorized.png`.

---

## 3-7 — unauthorised SSH access

Produced by the same `./exp_security.sh` run as 3-6 above — no separate
command. Tries password auth, root login, and an unknown account over SSH;
all three must be refused, and the authorised key must still work as a
control.

Screenshot the terminal output for 3-7 and save it as
`deliverables/screenshots/3-7_ssh_unauthorized.png`.

---

## 4-1 — guard mode

**Physical:** film the sequence; you need to appear in frame while armed.

Open `https://mahan.local/` and use the guard-mode toggle on the page. In a
second window, watch the alarm topic:

```bash
cd code/mac && ./subscribe_all.sh
```

Sequence to record:

1. Guard mode **off** — walk into frame. A detection is logged, no alarm topic.
2. Toggle guard mode **on** from the web page.
3. Walk into frame again — expect an immediate email **with photo attached** and
   a message on `alarm/402170516/home`.
4. Toggle back **off**.

Save as `deliverables/videos/4-1_guard_mode.mp4`, plus:

- `deliverables/screenshots/4-1_alarm_mqtt.png` — the `subscribe_all.sh`
  output showing the alarm topic message
- `deliverables/screenshots/4-1_alert_email.png` — the alert email with its
  photo attachment

---

## 4-2 — the black box

Nothing physical required.

```bash
cd code/tests && ./exp_features.sh 4-2
```

Reads the SQLite detection history, confirms the ring buffer stays bounded at
`db_ring_size`, and checks `/api/v1/history` serves the same data.

Screenshot the terminal's schema / row-count / recent-rows output and save it
as `deliverables/screenshots/4-2_black_box.png`.

---

## 4-3 — software watchdog

Nothing physical required — the capture link is cut in software, not by hand.
Takes a few minutes (`watchdog_stale_sec` + 25 s):

```bash
cd code/tests && ./exp_features.sh 4-3
```

Blocks the capture link at the firewall, waits for the watchdog to notice,
and confirms the detector restarts, the tampering email sends, and the main
daemon stays up throughout.

Screenshot the terminal's watchdog-fired / restart / tampering-email lines
and save it as `deliverables/screenshots/4-3_watchdog.png`.

---

## 4-4 — adaptive thermal management

Nothing physical required. Loads all four cores for 3 minutes (plus a second
phase if the 70°C setpoint isn't reached — see the ⚠ note at the top of this
file):

```bash
cd code/tests && ./exp_features.sh 4-4
```

Screenshot the terminal's peak-temperature / thermal-level output (including
phase B, if it runs) and save it as
`deliverables/screenshots/4-4_thermal_governor.png`.

---

## Screenshot checklist

One still per item below goes in the report. Work down the list in order and
check these off as you save them into `deliverables/screenshots/`:

- [ ] `1-1_boot_time.png` — terminal: systemd-analyze + blame + critical-chain
- [ ] `1-2_web_restart.png` — terminal: kill -9 → restart → journal tail
- [ ] `1-3_cold_boot_timestamp.png` — terminal: `uptime -s` / `systemd-analyze`
      right after the power cycle
- [ ] `1-4_https_redirect.png` — browser devtools: the 301 redirect
- [ ] `1-5_certificate.png` — browser: the padlock / certificate dialog
- [ ] `1-6_html_page.png` — browser: the dashboard page, title bar visible
- [ ] `2-1_thermal_states.png` — terminal: idle / stream-only / stream+detect table
- [ ] `2-2_memory_growth.png` — terminal: RSS summary and slope
- [ ] `2-3_api_load.png` — terminal: 50-request latency summary
- [ ] `2-4_network_drop.png` — terminal: the journalctl reconnect evidence
- [ ] `3-1_daylight.png`
- [ ] `3-1_artificial.png`
- [ ] `3-1_lowlight.png`
- [ ] `3-1_backlight.png`
- [ ] `3-2_spoof_printed.png`
- [ ] `3-2_spoof_screen.png`
- [ ] `3-3_resolution_sweep.png` — terminal: the FPS/temp/memory/detection table
- [ ] `3-4_lwt_outage.png` — terminal: captured will + reconnection
- [ ] `3-5_latency.png` — terminal: the mean/sd/min/max summary line
- [ ] `3-6_mqtt_unauthorized.png` — terminal: refused MQTT attempts
- [ ] `3-7_ssh_unauthorized.png` — terminal: refused SSH attempts
- [ ] `4-1_alarm_mqtt.png` — the `subscribe_all.sh` output showing the alarm
      topic message
- [ ] `4-1_alert_email.png` — the alert email with photo attachment
- [ ] `4-2_black_box.png` — terminal: schema / row count / recent rows
- [ ] `4-3_watchdog.png` — terminal: watchdog fired / restart / tampering email
- [ ] `4-4_thermal_governor.png` — terminal: peak temp / thermal level (+ phase B)

---

## Where things land

| what | where |
|---|---|
| script output, CSVs, transcripts | `code/tests/results/<id>/` |
| videos | `deliverables/videos/` |
| screenshots | `deliverables/screenshots/` |
| configs, certs | `deliverables/configs/` |

Each script writes a `result.md` the report quotes directly, so once these are
run the report assembles from the same files as the automated ones.
