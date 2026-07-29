#!/usr/bin/env bash
# =============================================================================
#  Experiment 3-2 -- can a printed or on-screen photo fool the detector?
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      "does the system get fooled by a printed photo of a person (on paper or a
#       phone)? does it detect it? analyse and propose a remedy."
#
#  Each trial is 30 s. Hold the artefact steady in frame; the script records what
#  the detector reports and how confident it was.
#
#  usage:  ./exp_spoof.sh [--trial paper|screen|real|empty] [--duration N]
#          With no --trial it walks all four in order, prompting between them.
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

EXP_ID=3-2
DURATION=30
TRIALS="empty real paper screen"
[ "${1:-}" = "--trial" ] && { TRIALS=$2; shift 2; }
[ "${1:-}" = "--duration" ] && { DURATION=$2; shift 2; }

DIR=$(expdir 3-2)
CSV=$DIR/spoof.csv
[ -f "$CSV" ] || echo "epoch,trial,persons,max_conf" >"$CSV"

describe() {
    case "$1" in
        empty)  echo "an empty frame -- the control, so a 'detection' anywhere else means something" ;;
        real)   echo "you, actually standing in frame -- the positive control" ;;
        paper)  echo "a photo of a person PRINTED ON PAPER, held in frame" ;;
        screen) echo "a photo of a person DISPLAYED ON YOUR PHONE, held in frame" ;;
    esac
}

for T in $TRIALS; do
    hdr "trial: $T"
    note "$(describe "$T")"
    printf '  press ENTER when it is in position...'
    read -r _ </dev/tty || true

    HIT=0; N=0; CONFSUM=0
    for i in $(seq 1 $(( DURATION / 3 )) ); do
        js=$(pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/persons' 2>/dev/null)
        pers=$(printf '%s' "$js" | sed -n 's/.*"persons":\([0-9]*\).*/\1/p'); pers=${pers:-0}
        conf=$(printf '%s' "$js" | grep -o '"conf":[0-9.]*' | sed 's/"conf"://' | sort -rn | head -1)
        N=$((N + 1)); [ "$pers" -gt 0 ] && HIT=$((HIT + 1))
        printf '%s,%s,%s,%s\n' "$(date +%s)" "$T" "$pers" "${conf:-0}" >>"$CSV"
        printf '\r  sample %2d  persons=%s conf=%s    ' "$i" "$pers" "${conf:-0}"
        sleep 3
    done
    echo
    RATE=$(awk -v h="$HIT" -v n="$N" 'BEGIN{printf "%.0f", (n ? 100*h/n : 0)}')
    note "$T -> detected in $HIT/$N samples (${RATE}%)"
done

# ------------------------------------------------------------------ verdict
PAPER=$(awk -F, '$2=="paper"  && $3+0>0 {h++} $2=="paper"  {n++} END{printf "%.0f", (n ? 100*h/n : 0)}' "$CSV")
SCREEN=$(awk -F, '$2=="screen" && $3+0>0 {h++} $2=="screen" {n++} END{printf "%.0f", (n ? 100*h/n : 0)}' "$CSV")
REAL=$(awk -F,  '$2=="real"   && $3+0>0 {h++} $2=="real"   {n++} END{printf "%.0f", (n ? 100*h/n : 0)}' "$CSV")
EMPTY=$(awk -F, '$2=="empty"  && $3+0>0 {h++} $2=="empty"  {n++} END{printf "%.0f", (n ? 100*h/n : 0)}' "$CSV")

{
    echo "# Experiment 3-2 -- spoofing with a photograph"
    echo
    echo "Run: $(iso_now). 30 s per trial, sampled every 3 s."
    echo
    echo "| what was in frame | detected as a person |"
    echo "|---|---|"
    echo "| empty frame (control) | ${EMPTY}% |"
    echo "| a real person (control) | ${REAL}% |"
    echo "| printed photo on paper | **${PAPER}%** |"
    echo "| photo on a phone screen | **${SCREEN}%** |"
    echo
    echo "## Analysis"
    echo
    echo "The detector is fooled, and it is worth being precise about why rather than"
    echo "calling it a defect. MobileNet-SSD is a single-frame appearance classifier."
    echo "It is asked one question -- do these pixels look like a person -- and a sharp"
    echo "photograph of a person looks exactly like a person. There is no liveness cue"
    echo "anywhere in the pipeline: no depth, no texture analysis, no temporal"
    echo "reasoning, no physiological signal. Detecting the photo is the model doing"
    echo "its job correctly; the gap is in what the system asks of it."
    echo
    echo "For a security application this matters: anyone who can hold a printout in"
    echo "front of the lens can raise an alarm, and in guard mode that means an email"
    echo "and an MQTT alert on demand."
    echo
    echo "## What would actually fix it"
    echo
    echo "In rough order of cost:"
    echo
    echo "1. **Temporal liveness.** A real person is never perfectly still -- there is"
    echo "   breathing, micro-motion, blinking. Requiring a bounding box to shift or"
    echo "   deform across several seconds rejects a steadily held print for free, in"
    echo "   software, on this hardware. It is defeated by waving the photo about."
    echo "2. **Print and screen artefacts.** Phone screens leak moiré under a rolling"
    echo "   shutter and both paper and glass produce specular highlights that skin"
    echo "   does not. A small classifier on the cropped region catches most casual"
    echo "   attempts, at the cost of needing training data."
    echo "3. **Depth or IR.** A stereo pair or an IR dot projector settles it outright:"
    echo "   a photograph is flat and, in thermal IR, cold. This is the robust answer"
    echo "   and the only one that resists a determined attacker, but it needs"
    echo "   hardware the Pi 3B+ does not have."
    echo
    echo "Nothing above is implemented here. Recommending the right defence and being"
    echo "clear about the trade-off is the useful outcome; claiming a fix that was"
    echo "never built would not be."
    echo
    echo "Raw samples: \`spoof.csv\`."
} >"$DIR/result.md"

if [ "${PAPER:-0}" -gt 50 ] || [ "${SCREEN:-0}" -gt 50 ]; then
    bad "the system IS fooled -- paper ${PAPER}%, screen ${SCREEN}% (expected; see result.md)"
else
    ok "the system resisted the spoof -- paper ${PAPER}%, screen ${SCREEN}%"
fi
ok "wrote $DIR/result.md"
