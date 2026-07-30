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
#  pulls from the DVR, decodes, downscales, and re-encodes to MJPEG for the board
#  -- which is exactly the cheap format the board already handled comfortably
#  before the DVR existed. Nothing on the board changes: it goes back to
#  camera_source=socket and the same guardian-ingest.socket path that was
#  already built and tested.
#
#  An earlier version of this file claimed the laptop could take the 1080p H.265
#  MAIN stream and hand the board a better downscale of it. That turned out to be
#  wrong twice over, and both corrections are recorded at the STREAM and -hwaccel
#  settings below: this DVR cannot deliver the main stream without packet loss,
#  and VideoToolbox cannot decode its video reliably even when the bitstream is
#  clean. The sub-stream in software is what actually works.
#
#  No detail is lost by that. The wire is 640px wide either way, so after the
#  detector's square crop the subject occupies the same number of pixels in the
#  300x300 network input regardless of which source stream it came from.
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
# sub, not main. The 1080p H.265 main stream does not arrive intact from this
# DVR: RTSP reports large sequence jumps ("RTP: PT=60: bad cseq"), the reference
# chain never closes, and every frame fails with "Error constructing the frame
# RPS". The same DVR answers 453 Not Enough Bandwidth under load, so it simply
# cannot sustain that bitrate. Measured over 35 s per configuration:
#
#     main + hardware decode   800+ errors, packet loss
#     main + software decode   800  errors, packet loss   <- not the decoder
#     sub  + hardware decode    26  errors, 16 hwaccel failures
#     sub  + software decode     2  benign join messages   <- this
#
# Detection loses nothing: the wire is downscaled to 640px either way, so the
# subject ends up the same size inside the 300x300 network input.
STREAM=sub                 # main = H.265 1080p, sub = H.264 960x576
SIZE=640x384               # 5:3, matching the sub-stream's 960x576 exactly --
                           # scaling it to 16:9 would stretch every person
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

# --- single instance ---------------------------------------------------------
# Two copies of this script are worse than useless: both the DVR channel and the
# board's ingest socket (Backlog=1, single consumer) hold one client each, so the
# loser gets an RTSP session with no H.265 parameter sets and every frame fails
# to build its Reference Picture Set. That surfaces as an endless wall of
# "Could not find ref with POC N" / "Error constructing the frame RPS", which
# looks like a broken camera or a codec bug and says nothing about the real
# cause. It cost a debugging session to work out, so it fails loudly here now.
#
# A PID file rather than flock(1), which macOS does not ship. `kill -0` probes
# liveness so a stale file left by a killed process cannot block a fresh start.
LOCKFILE=${TMPDIR:-/tmp}/guardian-stream-dvr.pid
if [ -f "$LOCKFILE" ]; then
    OTHER=$(cat "$LOCKFILE" 2>/dev/null || echo "")
    if [ -n "$OTHER" ] && kill -0 "$OTHER" 2>/dev/null; then
        cat >&2 <<EOF
[dvr] another copy of this script is already running as PID $OTHER.

Running two would starve both of them. Stop the other one first:

    kill $OTHER

or, if you want to take over regardless:

    pkill -f stream_dvr.sh && pkill -f 'ffmpeg.*Streaming/Channels'
EOF
        exit 1
    fi
    echo "[dvr] clearing a stale lock from dead PID ${OTHER:-unknown}"
fi
echo $$ >"$LOCKFILE"
# Remove the lock on any exit, and take the ffmpeg child down with us -- an
# orphaned ffmpeg would keep holding the DVR channel with nothing supervising it.
cleanup() {
    rm -f "$LOCKFILE" "${ERRLOG:-}"
    [ -n "${FFPID:-}" ] && kill "$FFPID" 2>/dev/null
}
trap cleanup EXIT

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
echo "[dvr] decode  : software (libavcodec) on this laptop"
echo "[dvr] wire    : MJPEG $SIZE @ ${FPS}fps, q=$QUALITY"
echo "[dvr] target  : tcp://$HOST:$PORT"

if [ "$PROBE_ONLY" = 1 ]; then
    echo "[dvr] probing the source for 3 s ..."
    if timeout 30 ffmpeg -hide_banner -loglevel error \
            -rtsp_transport tcp -i "$URL" \
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

# Something else may already be feeding the board -- the RTSP path from the board
# itself, or a copy of this script running on another machine, neither of which
# the PID file above can see. Only ESTAB counts: this script's own previous runs
# leave connections in CLOSE-WAIT for a while, and treating those as a live
# competitor would refuse to start after every normal restart.
PEER=$(ssh -o BatchMode=yes -o ConnectTimeout=6 "mahan@$HOST" \
        "ss -tn state established '( sport = :$PORT )' 2>/dev/null | tail -n +2" 2>/dev/null)
if [ -n "$PEER" ]; then
    echo >&2
    echo "[dvr] the board's ingest port already has a live feed:" >&2
    printf '        %s\n' "$PEER" >&2
    echo "[dvr] Two producers would starve each other. Stop the other one, or if" >&2
    echo "[dvr] that is the board's own RTSP path, it is already getting video." >&2
    exit 1
fi

echo "[dvr] note    : joining mid-GOP produces a few decoder warnings until the"
echo "[dvr]           first keyframe. They are suppressed and counted, not a fault."
if [ "$STREAM" = main ]; then
    echo "[dvr] WARNING : the main stream is selected. This DVR does not deliver it"
    echo "[dvr]           intact -- expect packet loss and a continuous stream of"
    echo "[dvr]           'Error constructing the frame RPS'. Use --stream sub."
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
        `# No -hwaccel. VideoToolbox failed on this DVR's video with` \
        `# "vt decoder cb: output image buffer is null: -12909" (that is` \
        `# kVTVideoDecoderMalfunctionErr) followed by "hardware accelerator` \
        `# failed to decode picture" -- 16 failures in 35 s even on the` \
        `# sub-stream, where the bitstream itself is clean. Software decode of` \
        `# 960x576 H.264 is trivial on Apple silicon and gave 0 failures.` \
        -i "$URL" \
        -vf "fps=$FPS,scale=${SIZE/x/:}:flags=bicubic,format=yuvj420p" \
        -c:v mjpeg -q:v "$QUALITY" -f mjpeg \
        "tcp://$HOST:$PORT" 2>&1 | {
            # Joining an H.265 stream mid-GOP always errors until the first
            # keyframe arrives: the decoder has no parameter sets yet, so it
            # cannot resolve references. Once a keyframe lands the errors stop
            # completely -- measured 0.00/s over 60 s of steady state.
            #
            # The window is 45 s because this DVR's main stream has a long
            # keyframe interval: measured joins took 25-30 s and produced ~500
            # warnings. (The DVR does not report keyFrameInterval over ISAPI, so
            # this is empirical.) An earlier 8 s window expired mid-join and let
            # the tail through, which looked exactly like the failure it was
            # meant to hide.
            #
            # Filtered by wall clock, and only for the specific decoder messages
            # known to be join artefacts -- anything else passes through at once,
            # so a real fault still reaches the user immediately. Lowering
            # -loglevel for the whole run would have hidden genuine errors too.
            QUIET_UNTIL=$(( $(date +%s) + 45 ))
            suppressed=0
            while IFS= read -r line; do
                case "$line" in
                    *"Could not find ref with POC"*|*"Error constructing the frame RPS"*|\
                    *"Skipping invalid undecodable NALU"*|*"PPS id out of range"*|\
                    *"Last message repeated"*)
                        if [ "$(date +%s)" -lt "$QUIET_UNTIL" ]; then
                            suppressed=$((suppressed + 1))
                            printf '%s\n' "$line" >>"$ERRLOG"
                            continue
                        fi
                        ;;
                esac
                if [ "$suppressed" -gt 0 ]; then
                    echo "[dvr] (suppressed $suppressed decoder warnings while the" \
                         "stream reached its first keyframe -- this is normal)"
                    suppressed=0
                fi
                printf '%s\n' "$line" | tee -a "$ERRLOG"
            done
            [ "$suppressed" -gt 0 ] && echo "[dvr] (suppressed $suppressed decoder" \
                "warnings during stream join -- normal)"
        }

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
