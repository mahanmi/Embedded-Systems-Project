#!/usr/bin/env bash
# =============================================================================
#  DVR -> laptop -> board video link
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  WHY THIS EXISTS
#  ---------------
#  The board can pull RTSP from the DVR by itself (see camera_source=rtsp), and
#  it works -- but H.264 decoding costs about 57% of one core on a Pi 3B+, and
#  the Pi has no usable hardware decoder from this OpenCV build. That extra load
#  pushed the board into constant thermal throttling: 24 governor events in six
#  hours, cycling between full rate and 2 fps.
#
#  This script moves the decode to the machine that can afford it. The laptop
#  pulls from the DVR, decodes in hardware through VideoToolbox, downscales, and
#  re-encodes to MJPEG for the board -- which is exactly the cheap format the
#  board already handled comfortably before the DVR existed. Nothing on the
#  board changes: it goes back to camera_source=socket and the same
#  guardian-ingest.socket path that was already built and tested.
#
#  Because the decode happens here, quality can go UP rather than down. The
#  board was limited to the 960x576 H.264 sub-stream; this laptop can decode the
#  1920x1080 H.265 MAIN stream without noticing, then hand the board a clean
#  downscale of it. A 640-wide frame derived from 1080p carries visibly more
#  detail than one derived from 960x576, which matters for a detector that has
#  to find people at a distance.
#
#  CREDENTIALS
#  -----------
#  The camera password is read from .secrets/camera.env and never appears in
#  this file or in the repository. One caveat worth stating plainly: RTSP has no
#  separate credential flags, so the password necessarily reaches ffmpeg inside
#  the URL argument, which is visible in `ps` while the stream runs. The board
#  side avoids this by building the URL inside the process; here it is
#  unavoidable. On a single-user laptop the exposure is small, but it is real.
#
#  usage:  ./stream_dvr.sh [--channel 1|2] [--stream main|sub] [--size WxH]
#                          [--fps N] [--quality N] [--host IP] [--probe]
# =============================================================================
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

HOST=${GUARDIAN_HOST:-192.168.100.26}
PORT=${GUARDIAN_INGEST_PORT:-9000}
CAM_HOST=${CAMERA_HOST:-192.168.100.64}
CAM_PORT=${CAMERA_RTSP_PORT:-554}
CAM_USER=${CAMERA_USER:-admin}
CHANNEL=2                  # 2 = KOOCHE (alley), 1 = HAYAT (yard)
STREAM=main                # main = H.265 1080p, sub = H.264 960x576
SIZE=640x360               # what the board receives; 16:9 to match the main stream
FPS=12                     # wire rate; the detector consumes ~7
QUALITY=4                  # ffmpeg -q:v, 2 (best) .. 31 (worst)
PROBE_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --channel) CHANNEL=$2; shift 2 ;;
        --stream)  STREAM=$2;  shift 2 ;;
        --size)    SIZE=$2;    shift 2 ;;
        --fps)     FPS=$2;     shift 2 ;;
        --quality) QUALITY=$2; shift 2 ;;
        --host)    HOST=$2;    shift 2 ;;
        --port)    PORT=$2;    shift 2 ;;
        --probe)   PROBE_ONLY=1; shift ;;
        -h|--help) sed -n '2,38p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

command -v ffmpeg >/dev/null || { echo "ffmpeg is not installed" >&2; exit 1; }

# --- credentials -------------------------------------------------------------
SECRETS=$HERE/.secrets/camera.env
if [ ! -f "$SECRETS" ]; then
    cat >&2 <<EOF
No camera credentials at $SECRETS

Create it with (it is already covered by .gitignore):

    mkdir -p "$HERE/.secrets"
    printf 'CAMERA_PASS=%s\n' 'YOUR_DVR_PASSWORD' > "$SECRETS"
    chmod 600 "$SECRETS"
EOF
    exit 1
fi
# shellcheck source=/dev/null
. "$SECRETS"
[ -n "${CAMERA_PASS:-}" ] || { echo "CAMERA_PASS is empty in $SECRETS" >&2; exit 1; }

# Hikvision numbering: <channel><stream>, stream 1 = main, 2 = sub.
case "$STREAM" in
    main) TRACK="${CHANNEL}01" ;;
    sub)  TRACK="${CHANNEL}02" ;;
    *) echo "--stream must be main or sub" >&2; exit 2 ;;
esac
URL="rtsp://$CAM_USER:$CAMERA_PASS@$CAM_HOST:$CAM_PORT/Streaming/Channels/$TRACK"
SAFE_URL="rtsp://$CAM_USER:***@$CAM_HOST:$CAM_PORT/Streaming/Channels/$TRACK"

case "$CHANNEL" in
    1) CAM_NAME="HAYAT (yard)" ;;
    2) CAM_NAME="KOOCHE (alley)" ;;
    *) CAM_NAME="channel $CHANNEL" ;;
esac

echo "[dvr] source  : $CAM_NAME $STREAM stream -- $SAFE_URL"
echo "[dvr] decode  : VideoToolbox (hardware) on this laptop"
echo "[dvr] wire    : MJPEG $SIZE @ ${FPS}fps, q=$QUALITY"
echo "[dvr] target  : tcp://$HOST:$PORT"

if [ "$PROBE_ONLY" = 1 ]; then
    echo "[dvr] probing the source for 3 s ..."
    if timeout 30 ffmpeg -hide_banner -loglevel error \
            -rtsp_transport tcp -hwaccel videotoolbox -i "$URL" \
            -frames:v 30 -f null - 2>/dev/null; then
        echo "[dvr] source OK"
    else
        echo "[dvr] could not read the source" >&2; exit 1
    fi
    echo "[dvr] --probe given; not streaming"
    exit 0
fi

# The board must be listening, or every frame is wasted encoding effort.
if ! nc -z -G 3 "$HOST" "$PORT" 2>/dev/null; then
    echo "[dvr] warning: $HOST:$PORT is not accepting connections yet." >&2
    echo "[dvr]          Is camera_source=socket on the board? The ingest socket" >&2
    echo "[dvr]          is only bound in socket mode. Retrying anyway." >&2
fi

echo "[dvr] press Ctrl-C to stop"
ERRLOG=$(mktemp -t guardian-dvr) || exit 1
trap 'rm -f "$ERRLOG"' EXIT
trap 'echo; echo "[dvr] stopped"; exit 0' INT TERM

attempt=0
backoff=3
while true; do
    attempt=$((attempt + 1))
    echo "[dvr] connecting (attempt $attempt) ..."

    # -rtsp_transport tcp: UDP loses packets on a busy wifi link and H.265
    #   recovers from that far worse than H.264 does -- a single lost reference
    #   frame smears until the next keyframe.
    # -fflags nobuffer: do not let the demuxer sit on frames. Latency matters
    #   here; this link is in front of a detector.
    # NOTE: -flags low_delay was here and has been removed. With H.265 it made
    #   the decoder refuse to wait for reference frames, so it could not build
    #   the Reference Picture Set: ~4.8 errors/second, 601 "Skipping invalid
    #   undecodable NALU" and 598 "Error constructing the frame RPS" over six
    #   minutes -- about a tenth of the source frames thrown away. Latency was
    #   already 0.06-0.17 s against a 30 s watchdog, so there was nothing to buy
    #   and a working reference chain to lose.
    # fps filter before scale: dropping frames first means the scaler only
    #   works on frames we are actually going to send.
    ffmpeg -hide_banner -loglevel warning \
        -rtsp_transport tcp -fflags nobuffer \
        -hwaccel videotoolbox \
        -i "$URL" \
        -vf "fps=$FPS,scale=${SIZE/x/:}:flags=bicubic,format=yuvj420p" \
        -c:v mjpeg -q:v "$QUALITY" -f mjpeg \
        "tcp://$HOST:$PORT" 2>&1 | tee "$ERRLOG"

    rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] && { echo "[dvr] ffmpeg finished cleanly"; exit 0; }

    # Classify before retrying: a wrong password or a missing channel will fail
    # identically forever, and spinning on it just hides the message.
    if grep -qiE "401|unauthorized|authentication" "$ERRLOG"; then
        echo >&2
        echo "[dvr] the DVR rejected the credentials. Check CAMERA_PASS in" >&2
        echo "[dvr] $SECRETS and that the account may view channel $CHANNEL." >&2
        exit 1
    fi
    if grep -qiE "404|not found|no route to host" "$ERRLOG"; then
        echo >&2
        echo "[dvr] the DVR refused the stream path $TRACK." >&2
        echo "[dvr] Channels 3 and 4 report NO VIDEO on this recorder; try" >&2
        echo "[dvr] --channel 1 or --channel 2, or --stream sub." >&2
        exit 1
    fi
    if grep -qiE "connection refused" "$ERRLOG"; then
        echo "[dvr] the board is not listening on $PORT -- is camera_source=socket?" >&2
    fi

    echo "[dvr] ffmpeg exited (rc=$rc); retrying in ${backoff}s"
    sleep "$backoff"
    [ "$backoff" -lt 15 ] && backoff=$((backoff + 3))
done
