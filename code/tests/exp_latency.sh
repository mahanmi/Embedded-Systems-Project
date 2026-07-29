#!/usr/bin/env bash
# =============================================================================
#  Experiment 3-5 -- end-to-end latency, person in frame to MQTT on the laptop
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      "measure the delay from the moment a person enters the frame until the
#       MQTT message is received on the computer (by comparing timestamps);
#       report the mean and standard deviation over 10 measurements."
#
#  WHY THE CLOCK OFFSET IS MEASURED FIRST
#  --------------------------------------
#  The measurement subtracts a timestamp written by the board from a time read
#  on this laptop. Those are two different clocks, so any error between them
#  lands directly in the answer -- and at the scale being measured (hundreds of
#  ms) even a modest NTP disagreement would dominate. So the offset is estimated
#  first, by the usual round-trip method, and subtracted:
#
#      offset ~= t_board - (t_before + t_after)/2
#
#  The residual uncertainty is roughly half the SSH round-trip, which is
#  reported alongside the result so the reader can judge it. Without this the
#  numbers would look precise and be wrong.
#
#  WHAT IS ACTUALLY BEING TIMED
#  ----------------------------
#  From the timestamp the board stamped on the detection to the moment the
#  payload arrived here: inference, JSON assembly, MQTT publish, broker, and the
#  hop back. It excludes the time the person was in frame but not yet inferred,
#  which on this board is up to one motion-gate interval -- so this is a
#  publish-path latency, and the report says so rather than claiming it is the
#  full human-to-alert time.
#
#  usage:  ./exp_latency.sh [--samples N]
#          Walk into frame, wait for the beep/line, step out, repeat.
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

EXP_ID=3-5
SAMPLES=10
while [ $# -gt 0 ]; do
    case "$1" in
        --samples) SAMPLES=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

SUB=${SUB:-/opt/homebrew/opt/mosquitto/bin/mosquitto_sub}
BROKER=${BROKER:-127.0.0.1}
DIR=$(expdir 3-5)
CSV=$DIR/latency.csv
LOG=$DIR/cmd.log
: >"$LOG"

# shellcheck source=/dev/null
[ -f "$TESTS_DIR/../mac/.secrets/mqtt.env" ] && . "$TESTS_DIR/../mac/.secrets/mqtt.env"
VUSER=${MQTT_VIEWER_USER:-viewer}
VPASS=${MQTT_VIEWER_PASS:-}
[ -n "$VPASS" ] || { bad "no MQTT viewer password in mac/.secrets/mqtt.env"; exit 1; }

# ---------------------------------------------------------- clock offset
hdr "estimating the laptop/board clock offset"
OFFSETS=""
for _ in 1 2 3 4 5; do
    T0=$(python3 -c 'import time;print(f"{time.time():.6f}")')
    TB=$(pi 'date +%s.%N' 2>/dev/null)
    T1=$(python3 -c 'import time;print(f"{time.time():.6f}")')
    [ -n "$TB" ] || continue
    OFFSETS="$OFFSETS$(python3 -c "print(f'{$TB - ($T0 + $T1)/2:.6f}')")
"
done
OFFSET=$(printf '%s' "$OFFSETS" | awk 'NF{v[n++]=$1} END{ if(!n){print 0; exit}
    # Median, not mean: one slow SSH round trip should not drag the estimate.
    for(i=0;i<n;i++) for(j=i+1;j<n;j++) if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t}
    printf "%.6f", (n%2 ? v[int(n/2)] : (v[n/2-1]+v[n/2])/2) }')
RTT=$(python3 -c "print(f'{($T1 - $T0)*1000:.1f}')")
note "board clock is ${OFFSET}s relative to this laptop (last RTT ${RTT} ms)"
printf 'clock offset: %ss, last RTT: %s ms\n' "$OFFSET" "$RTT" >>"$LOG"

# ---------------------------------------------------------- collect events
echo "n,recv_epoch,msg_epoch,persons,raw_delta_s,corrected_delta_s" >"$CSV"
hdr "collecting $SAMPLES detection events"
note "walk INTO frame, wait for the PASS line, then step OUT. Repeat."
echo

# Only 0 -> N transitions count: a steady stream of "still 2 people" messages
# would otherwise be timed as if each were a fresh arrival.
"$SUB" -h "$BROKER" -u "$VUSER" -P "$VPASS" -t "persons/$STUDENT_ID/home" 2>/dev/null |
python3 -u -c '
import sys, json, time, math

samples   = int(sys.argv[1])
offset    = float(sys.argv[2])
csv_path  = sys.argv[3]

deltas, prev, n = [], 0, 0
with open(csv_path, "a") as csv:
    for line in sys.stdin:
        recv = time.time()
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        persons = int(msg.get("persons", 0))
        # Rising edge only: an empty scene becoming occupied.
        if persons > 0 and prev == 0:
            sent = float(msg.get("epoch", 0))
            if sent:
                raw = recv - sent
                corrected = raw - offset
                n += 1
                deltas.append(corrected)
                csv.write(f"{n},{recv:.6f},{sent:.6f},{persons},{raw:.4f},{corrected:.4f}\n")
                csv.flush()
                print(f"  \033[1;32mPASS\033[0m  sample {n}/{samples}: {corrected*1000:.0f} ms"
                      f" ({persons} person(s)) -- step out of frame", flush=True)
        prev = persons
        if n >= samples:
            break

if deltas:
    mean = sum(deltas) / len(deltas)
    sd   = math.sqrt(sum((d - mean) ** 2 for d in deltas) / len(deltas)) if len(deltas) > 1 else 0.0
    lo, hi = min(deltas), max(deltas)
    print(f"\nmean {mean*1000:.0f} ms   sd {sd*1000:.0f} ms   min {lo*1000:.0f} ms   max {hi*1000:.0f} ms")
    with open(csv_path + ".summary", "w") as f:
        f.write(f"{mean:.6f} {sd:.6f} {lo:.6f} {hi:.6f} {len(deltas)}\n")
else:
    print("no rising-edge detections were captured")
' "$SAMPLES" "$OFFSET" "$CSV"

# ---------------------------------------------------------------- summary
if [ -f "$CSV.summary" ]; then
    read -r MEAN SD LO HI N <"$CSV.summary"
    MS() { awk -v v="$1" 'BEGIN{printf "%.0f", v*1000}'; }
    cat >"$DIR/result.md" <<EOF
# Experiment 3-5 -- detection to MQTT latency

Run: $(iso_now). $N rising-edge events (an empty scene becoming occupied).

| | ms |
|---|---|
| mean | $(MS "$MEAN") |
| standard deviation | $(MS "$SD") |
| min | $(MS "$LO") |
| max | $(MS "$HI") |

Measured from the timestamp the board stamped on the detection to the arrival of
the payload on the laptop, corrected for a measured clock offset of
\`${OFFSET}s\` (median of five round-trip estimates; last RTT ${RTT} ms). The
correction matters: the two ends keep independent clocks, and at this scale an
uncorrected NTP disagreement would be larger than the quantity being measured.

Only 0 -> N transitions are timed. Counting every \`persons\` message would time
"still two people in the room" as though someone had just walked in, and would
report a latency far lower than the truth.

This is the **publish path**: inference, JSON assembly, MQTT publish at QoS 1,
broker, and the hop back. It excludes the interval between the person physically
entering and the frame being inferred, which on this board can reach one
motion-gate period -- the honest total for a human walking in is this figure plus
that wait, and both are reported rather than folded together.

Raw samples: \`latency.csv\`.
EOF
    ok "wrote $DIR/result.md"
else
    bad "no samples collected; is the stream running and is anyone in frame?"
fi
