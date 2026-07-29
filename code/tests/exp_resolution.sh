#!/usr/bin/env bash
# =============================================================================
#  Experiment 3-3 -- input resolution against FPS, temperature, memory, accuracy
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      "change the input resolution at three levels; report FPS, CPU temperature
#       after 5 minutes, memory use and accuracy at each, and conclude which is
#       the optimal operating point."
#
#  The swept parameter is the detector network's input edge, not the capture
#  size. That is the one that actually costs: MobileNet-SSD's work scales with
#  the square of its input edge, while the capture size only changes a memcpy
#  and a resize. Sweeping capture size would mostly measure the JPEG decoder.
#
#  ON ACCURACY
#  -----------
#  FPS, temperature and memory below are measured. "Accuracy" here is NOT: with
#  no labelled ground truth, what is recorded is the detection rate and mean
#  confidence while a person stays in frame. That is a fair *relative* indicator
#  across resolutions -- the same scene, the same person, only the input edge
#  changing -- but it is not an absolute accuracy figure, and the report says so.
#  Run with --person to assert someone is in frame for the whole sweep.
#
#  usage:  ./exp_resolution.sh [--person] [--dur SECONDS]
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

EXP_ID=3-3
PERSON_PRESENT=0
DUR=300
LEVELS=${LEVELS:-"300 224 160"}

while [ $# -gt 0 ]; do
    case "$1" in
        --person) PERSON_PRESENT=1; shift ;;
        --dur)    DUR=$2; shift 2 ;;
        --levels) LEVELS=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

DIR=$(expdir 3-3)
CSV=$DIR/sweep.csv
LOG=$DIR/cmd.log
: >"$LOG"
echo "epoch,iso,net_input,temp_c,cpu_pct,rss_kb,fps,infer_ms,persons,max_conf,throttled" >"$CSV"

hdr "sweeping the detector input edge over: $LEVELS (${DUR}s each)"
[ "$PERSON_PRESENT" = 1 ] && note "a person is asserted to be in frame throughout" \
                          || note "no person asserted: the accuracy columns will be reported as unusable"

# Restore the configured default whatever happens.
DEFAULT_NET=$(pi "sudo grep -E '^target_net_input' /etc/guardian/guardian.conf | awk '{print \$3}'" 2>/dev/null)
DEFAULT_NET=${DEFAULT_NET:-300}
trap 'echo; note "restoring net_input=$DEFAULT_NET"; pi_api POST /api/v1/command "{\"cmd\":\"set_net_input\",\"net_input\":$DEFAULT_NET}" >/dev/null 2>&1; exit 130' INT TERM

for NET in $LEVELS; do
    hdr "net_input = ${NET}px"
    pi_api POST /api/v1/command "{\"cmd\":\"set_net_input\",\"net_input\":$NET}" >>"$LOG" 2>&1

    # Let the pipeline settle and the SoC reach the new steady state before
    # sampling: the first frames after a change still carry the old cost.
    sleep 30

    SAMPLES=$(( DUR / 10 ))
    for _ in $(seq 1 "$SAMPLES"); do
        js=$(pi 'curl -sk --max-time 5 https://127.0.0.1/api/v1/persons' 2>/dev/null)
        tj=$(pi 'curl -sk --max-time 5 https://127.0.0.1/api/v1/telemetry' 2>/dev/null)
        fps=$(printf   '%s' "$js" | sed -n 's/.*"fps":\([0-9.]*\).*/\1/p')
        inf=$(printf   '%s' "$js" | sed -n 's/.*"inference_ms":\([0-9.]*\).*/\1/p')
        pers=$(printf  '%s' "$js" | sed -n 's/.*"persons":\([0-9]*\).*/\1/p')
        # Highest confidence in this frame, or 0 when nothing was detected.
        conf=$(printf  '%s' "$js" | grep -o '"conf":[0-9.]*' | sed 's/"conf"://' |
               sort -rn | head -1)
        temp=$(printf  '%s' "$tj" | sed -n 's/.*"cpu_temp_c":\([0-9.]*\).*/\1/p')
        cpu=$(printf   '%s' "$tj" | sed -n 's/.*"cpu_percent":\([0-9.]*\).*/\1/p')
        rss=$(printf   '%s' "$tj" | sed -n 's/.*"daemon_rss_kb":\([0-9]*\).*/\1/p')
        thr=$(printf   '%s' "$tj" | sed -n 's/.*"thermal_level":\([0-9]*\).*/\1/p')
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$(iso_now)" "$NET" \
            "${temp:-NA}" "${cpu:-NA}" "${rss:-NA}" "${fps:-0}" "${inf:-0}" \
            "${pers:-0}" "${conf:-0}" "${thr:-NA}" >>"$CSV"
        sleep 10
    done

    LAST_TEMP=$(awk -F, -v n="$NET" '$3==n{t=$4} END{print t}' "$CSV")
    MEAN_FPS=$(awk -F, -v n="$NET" '$3==n && $7!="NA"{s+=$7;c++} END{if(c)printf "%.2f",s/c; else print "NA"}' "$CSV")
    note "net_input=$NET -> mean fps $MEAN_FPS, temp at end ${LAST_TEMP}C"
done

pi_api POST /api/v1/command "{\"cmd\":\"set_net_input\",\"net_input\":$DEFAULT_NET}" >>"$LOG" 2>&1
trap - INT TERM
ok "restored net_input=$DEFAULT_NET"

# ------------------------------------------------------------------ summary
{
    echo "# Experiment 3-3 -- detector input resolution sweep"
    echo
    echo "Run: $(iso_now). ${DUR}s per level after a 30 s settling period."
    echo
    echo "| net input | mean FPS | mean inference | temp at 5 min | daemon RSS | detection rate | mean peak conf |"
    echo "|---|---|---|---|---|---|---|"
    for NET in $LEVELS; do
        awk -F, -v n="$NET" '
            $3==n {
                c++;
                if ($7!="NA") { fs+=$7; fc++ }
                if ($8!="NA") { is+=$8; ic++ }
                if ($6!="NA") { rs+=$6; rc++ }
                if ($9+0 > 0) det++
                if ($10+0 > 0) { cs+=$10; cc++ }
                t=$4
            }
            END {
                printf "| %s px | %.2f | %.0f ms | %s C | %.0f kB | %.0f%% | %.2f |\n",
                    n,
                    (fc ? fs/fc : 0),
                    (ic ? is/ic : 0),
                    t,
                    (rc ? rs/rc : 0),
                    (c ? 100*det/c : 0),
                    (cc ? cs/cc : 0)
            }' "$CSV"
    done
    echo
    if [ "$PERSON_PRESENT" = 1 ]; then
        echo "A person was in frame for the whole sweep, so the detection-rate and"
        echo "confidence columns are comparable across the three levels. They remain a"
        echo "*relative* indicator, not an absolute accuracy: there is no labelled ground"
        echo "truth here, only the same scene measured three times."
    else
        echo "**The accuracy columns are not usable for this run** -- nobody was asserted to"
        echo "be in frame, so a 0% detection rate means only that the room was empty. Re-run"
        echo "with \`--person\` while standing in view to populate them. FPS, inference time,"
        echo "temperature and memory above are unaffected and remain valid."
    fi
    echo
    echo "Inference cost scales with the square of the input edge, so 224 px is"
    echo "roughly $(awk 'BEGIN{printf "%.0f", 100*224*224/(300*300)}')% of the work of 300 px and 160 px about"
    echo "$(awk 'BEGIN{printf "%.0f", 100*160*160/(300*300)}')%. The measured inference times above are what to check that"
    echo "prediction against -- they include fixed per-frame costs (JPEG decode, resize,"
    echo "overlay) that do not shrink, so the speed-up is always less than the ratio."
    echo
    echo "Raw data: \`sweep.csv\`."
} >"$DIR/result.md"

ok "wrote $DIR/result.md"
cat "$DIR/result.md" | sed -n '/| net input/,/^$/p'
