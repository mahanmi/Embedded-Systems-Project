#!/usr/bin/env bash
# =============================================================================
#  Capture host -> board video link
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  The Raspberry Pi 3B+ used for this project has no camera attached (its
#  /dev/video1x nodes are the VideoCore codec, not a capture device), so the
#  project brief's documented fallback applies: use the laptop's webcam and add
#  a stage that streams the picture to the board.
#
#  Transport is deliberately the simplest thing that works: raw MJPEG over a
#  plain TCP connection. No RTSP server, no container, no muxing -- just JPEG
#  images back to back, which the detector splits on the SOI/EOI markers. On a
#  100 Mbit link 640x480 at 15 fps is roughly 6 Mbit/s, comfortably within
#  budget, and it keeps end-to-end latency near one frame time (which matters
#  for experiment 3-5).
#
#  The listening socket on the board belongs to guardian-ingest.socket, so this
#  script can be started before, after, or repeatedly alongside the board's
#  services -- it simply reconnects.
#
#  ---------------------------------------------------------------------------
#  Why this script probes the camera instead of hardcoding capture settings
#  ---------------------------------------------------------------------------
#  ffmpeg's avfoundation input does not negotiate. It walks the device's list of
#  native modes and accepts one only if the requested frame rate equals that
#  mode's *maximum* rate:
#
#      fabs(requested - range.maxFrameRate) < 0.01      (libavdevice/avfoundation.m)
#
#  A camera advertising "640x480@[15 30]fps" therefore rejects -framerate 15
#  even though 15 fps is genuinely supported, and it rejects the ffmpeg default
#  too, because that default is "ntsc" = 29.97, which misses 30.0 by 0.03.
#  The pixel format is just as literal: Apple silicon FaceTime cameras publish
#  420v/420f/BGRA, while ffmpeg's default request is yuv420p ('y420' planar),
#  which those cameras do not offer.
#
#  Both failures surface as the same unhelpful "Input/output error", so instead
#  of guessing we ask the device once at startup: a throwaway open with an
#  impossible frame rate makes ffmpeg dump "Supported modes:", and a second one
#  with an impossible pixel format makes it dump "Supported pixel formats:".
#  We parse both and pick from them. That keeps the script working on any Mac.
#
#  Capture rate and wire rate are kept separate: we capture at whatever the
#  device demands (usually 30) and decimate to --fps with the fps filter, so the
#  bandwidth and the board's load stay at the 15 fps the experiments assume.
#
#  usage:  ./stream_webcam.sh [--device N] [--fps N] [--size WxH] [--host IP]
#          ./stream_webcam.sh --list     # enumerate capture devices
#          ./stream_webcam.sh --probe    # resolve capture settings and exit
# =============================================================================
set -uo pipefail

HOST=${GUARDIAN_HOST:-192.168.100.26}
PORT=${GUARDIAN_INGEST_PORT:-9000}
DEVICE=${GUARDIAN_CAM:-0}          # avfoundation index; 0 = FaceTime HD Camera
FPS=15                             # rate put on the wire
SIZE=640x480                       # frame size put on the wire
QUALITY=6                          # ffmpeg -q:v, 2 (best) .. 31 (worst)
CAP_FPS=""                         # capture rate; empty => probe the device
PIXFMT=""                          # capture pixel format; empty => probe
PROBE_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEVICE=$2; shift 2 ;;
        --fps)    FPS=$2;    shift 2 ;;
        --size)   SIZE=$2;   shift 2 ;;
        --host)   HOST=$2;   shift 2 ;;
        --port)   PORT=$2;   shift 2 ;;
        --quality) QUALITY=$2; shift 2 ;;
        --capture-fps)   CAP_FPS=$2; shift 2 ;;
        --pixel-format)  PIXFMT=$2;  shift 2 ;;
        --probe)  PROBE_ONLY=1; shift ;;
        --list)
            ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 |
                sed -n '/video devices/,/audio devices/p'
            exit 0 ;;
        -h|--help) sed -n '2,48p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

command -v ffmpeg >/dev/null || { echo "ffmpeg is not installed" >&2; exit 1; }

ERRLOG=$(mktemp -t guardian-cam) || exit 1
trap 'rm -f "$ERRLOG"' EXIT

# --- failure classification -------------------------------------------------
# Only transport failures are worth retrying. A permission or configuration
# failure repeats forever, so we say what is wrong and stop.
fatal_reason() {
    local log=$1
    if grep -qE "Failed to create AV capture input device|not authorized|Cannot use" "$log"; then
        cat >&2 <<EOF

[cam] the camera could not be opened. The two causes, in order of likelihood:

  1. This terminal has no camera permission.
     System Settings -> Privacy & Security -> Camera, enable your terminal
     (Terminal, iTerm, VS Code -- whichever you launched this from), then
     fully quit and reopen it. macOS only applies the grant on a fresh launch.

  2. Another application is holding the camera.
     Quit Zoom / FaceTime / Photo Booth / OBS and try again.

EOF
        return 0
    fi
    if grep -qE "Selected (framerate|video size|pixel format)" "$log"; then
        echo "[cam] the device rejected the capture settings; re-probing" >&2
        return 1   # recoverable: caller re-probes
    fi
    return 2       # transport error: retry
}

# --- probe 1: supported modes ----------------------------------------------
# An impossible frame rate makes avfoundation dump every mode it has. Lines
# look like:  [in#0 @ 0x..]   640x480@[15.000000 30.000000]fps
probe_modes() {
    ffmpeg -hide_banner -loglevel error \
        -f avfoundation -video_size "$SIZE" -framerate 1 -i "$DEVICE" \
        -frames:v 1 -f null - 2>&1 |
    awk '{
        if (match($0, /[0-9]+x[0-9]+@\[[0-9. ]+\]fps/)) {
            m = substr($0, RSTART, RLENGTH)
            split(m, a, "@")
            rates = a[2]
            gsub(/[][]|fps/, "", rates)
            n = split(rates, r, " ")
            print a[1], r[n]          # size, highest rate for that size
        }
    }'
}

# --- probe 2: supported pixel formats ---------------------------------------
# Same trick with a pixel format no webcam implements.
probe_pixfmts() {
    local cap_size=$1 cap_fps=$2
    ffmpeg -hide_banner -loglevel error \
        -f avfoundation -video_size "$cap_size" -framerate "$cap_fps" \
        -pixel_format rgb555be -i "$DEVICE" \
        -frames:v 1 -f null - 2>&1 |
    sed -n '/Supported pixel formats:/,$p' |
    sed -n 's/^.*\] *\([a-z0-9]\{3,\}\) *$/\1/p'
}

# Resolve CAP_SIZE / CAP_FPS / PIXFMT from the device. Sets the globals.
resolve_settings() {
    local modes want_fps
    modes=$(probe_modes)

    if [ -z "$modes" ]; then
        # The probe could not talk to the device at all -- almost always
        # permission. Fall back to sane values and let the real run report it.
        CAP_SIZE=$SIZE
        [ -n "$CAP_FPS" ] || CAP_FPS=30
        [ -n "$PIXFMT" ]  || PIXFMT=nv12
        echo "[cam] warning: mode probe returned nothing; assuming ${CAP_SIZE}@${CAP_FPS} ${PIXFMT}" >&2
        return
    fi

    # Prefer the requested size; otherwise the widest mode not larger than
    # 1280 wide, so we never haul 1080p frames across the wire to downscale.
    want_fps=$(echo "$modes" | awk -v s="$SIZE" '$1 == s { print $2; exit }')
    if [ -n "$want_fps" ]; then
        CAP_SIZE=$SIZE
    else
        CAP_SIZE=$(echo "$modes" | awk '
            { split($1, d, "x"); if (d[1] <= 1280 && d[1] >= d[2] && d[1] > best) { best = d[1]; sz = $1; fr = $2 } }
            END { if (sz) print sz; }')
        want_fps=$(echo "$modes" | awk -v s="$CAP_SIZE" '$1 == s { print $2; exit }')
        if [ -z "$CAP_SIZE" ]; then
            CAP_SIZE=$(echo "$modes" | awk 'NR == 1 { print $1 }')
            want_fps=$(echo "$modes" | awk 'NR == 1 { print $2 }')
        fi
        echo "[cam] note: $SIZE is not a native mode; capturing $CAP_SIZE and scaling" >&2
    fi
    # ffmpeg wants the mode's maximum rate, printed by the probe as e.g. 30.000000.
    [ -n "$CAP_FPS" ] || CAP_FPS=${want_fps%.*}

    if [ -z "$PIXFMT" ]; then
        local avail
        avail=$(probe_pixfmts "$CAP_SIZE" "$CAP_FPS")
        for candidate in nv12 uyvy422 yuyv422 yuv420p bgr0 0rgb bgra abgr rgb24 bgr24; do
            if echo "$avail" | grep -qx "$candidate"; then PIXFMT=$candidate; break; fi
        done
        [ -n "$PIXFMT" ] || PIXFMT=nv12
    fi
}

echo "[cam] probing avfoundation device $DEVICE ..."
resolve_settings

# Only rescale when the device could not give us the size we actually want.
VF="fps=$FPS"
[ "$CAP_SIZE" = "$SIZE" ] || VF="$VF,scale=${SIZE/x/:}"
VF="$VF,format=yuvj420p"

echo "[cam] source  : avfoundation device $DEVICE, $CAP_SIZE @ ${CAP_FPS}fps ($PIXFMT)"
echo "[cam] wire    : MJPEG $SIZE @ ${FPS}fps, q=$QUALITY"
echo "[cam] target  : tcp://$HOST:$PORT"

if [ "$PROBE_ONLY" = 1 ]; then
    echo "[cam] --probe given; not streaming"
    exit 0
fi

echo "[cam] press Ctrl-C to stop"
trap 'echo; echo "[cam] stopped"; exit 0' INT TERM

attempt=0
backoff=3
while true; do
    attempt=$((attempt + 1))
    echo "[cam] connecting (attempt $attempt) ..."

    # -f mjpeg writes bare concatenated JPEGs, which is exactly what the
    # detector's frame scanner expects.
    # listen=0 => we are the client; the board is already listening.
    ffmpeg -hide_banner -loglevel warning \
        -f avfoundation -framerate "$CAP_FPS" -video_size "$CAP_SIZE" \
        -pixel_format "$PIXFMT" -i "$DEVICE" \
        -vf "$VF" \
        -c:v mjpeg -q:v "$QUALITY" -f mjpeg \
        "tcp://$HOST:$PORT" 2>&1 | tee "$ERRLOG"

    # A pipeline waits for every member, so $ERRLOG is complete here -- unlike
    # `2> >(tee ...)`, where bash would not wait for the substitution and we
    # could classify a half-written log. pipefail + PIPESTATUS keep ffmpeg's
    # own exit status visible through the pipe.
    rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] && { echo "[cam] ffmpeg finished cleanly"; exit 0; }

    fatal_reason "$ERRLOG"
    case $? in
        0) exit 1 ;;                        # permission / device busy
        1) resolve_settings                 # settings drifted; re-resolve once
           VF="fps=$FPS"
           [ "$CAP_SIZE" = "$SIZE" ] || VF="$VF,scale=${SIZE/x/:}"
           VF="$VF,format=yuvj420p"
           echo "[cam] retrying with $CAP_SIZE @ ${CAP_FPS}fps ($PIXFMT)" ;;
        *) : ;;                             # transport error: plain retry
    esac

    echo "[cam] ffmpeg exited (rc=$rc); retrying in ${backoff}s"
    sleep "$backoff"
    # Gentle backoff so a board that is still booting does not spam the log.
    [ "$backoff" -lt 15 ] && backoff=$((backoff + 3))
done
