# Experiments that need you at the board

Everything else runs from `code/tests/`. These six need a physical action, a
camera pointed at something, or a video recording — so the commands below do the
measuring and you supply the part a script cannot.

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

Screenshot the terminal. Expected: total boot time, then the per-unit table with
`guardian`, `guardian-vision` and `guardian-api` visible.

> The detector dominates its own start-up because it loads a 23 MB Caffe model.
> That is worth one sentence in the report — it is start-up cost, not a stall.

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

Save as `deliverables/videos/1-3_cold_boot.mp4`.

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
and live clock, which is what the brief wants visible).

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

Capture a still of each attempt being detected (or not).

> Expect it **to be fooled**. MobileNet-SSD is a single-frame appearance detector
> with no liveness cue whatsoever — it has no notion of depth, texture, motion
> parallax or pulse, so a sharp photo is simply a person to it. The report should
> say this plainly and then propose remedies: depth or IR imaging, blink/motion
> liveness over consecutive frames, texture analysis for print/screen artefacts
> (moiré, specular reflection), or requiring parallax across a moving camera.
> Do not claim a fix you have not implemented — naming the right defence and
> explaining the trade-off is the answer here.

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

Save as `deliverables/videos/4-1_guard_mode.mp4`, plus a screenshot of the alarm
message and of the email with its attachment.

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
