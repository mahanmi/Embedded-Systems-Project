#!/usr/bin/env bash
# =============================================================================
#  Watch every topic the board publishes, with local receive timestamps.
#  This is the "observe the messages in the relevant topic" half of part 3-c.
#
#  usage:  ./subscribe_all.sh [--raw]
# =============================================================================
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
STUDENT_ID=${STUDENT_ID:-402170516}
BROKER=${BROKER:-127.0.0.1}
SUB=/opt/homebrew/opt/mosquitto/bin/mosquitto_sub

# Credentials come from the untracked secrets file, never from this script.
# shellcheck source=/dev/null
[ -f "$HERE/.secrets/mqtt.env" ] && source "$HERE/.secrets/mqtt.env"
USER_NAME=${MQTT_VIEWER_USER:-viewer}
PASS=${MQTT_VIEWER_PASS:-}

if [ -z "$PASS" ]; then
    echo "no MQTT password found in $HERE/.secrets/mqtt.env" >&2
    exit 1
fi

RAW=0
[ "${1:-}" = "--raw" ] && RAW=1

echo "subscribing to +/$STUDENT_ID/home on $BROKER as '$USER_NAME'"
echo "(topic, local receive time, payload)"
echo

if [ "$RAW" = 1 ]; then
    exec "$SUB" -h "$BROKER" -u "$USER_NAME" -P "$PASS" \
         -t "+/$STUDENT_ID/home" -q 1 -v
fi

"$SUB" -h "$BROKER" -u "$USER_NAME" -P "$PASS" \
       -t "+/$STUDENT_ID/home" -q 1 -v |
while IFS= read -r line; do
    topic=${line%% *}
    payload=${line#* }
    recv=$(python3 -c 'import time;print(f"{time.time():.3f}")')
    # Colour by topic so alarms stand out during the demo.
    case "$topic" in
        alarm/*)  colour='\033[1;31m' ;;
        status/*) colour='\033[1;33m' ;;
        persons/*) colour='\033[1;32m' ;;
        event/*)  colour='\033[1;35m' ;;
        *)        colour='\033[0;36m' ;;
    esac
    printf "${colour}%-28s\033[0m %s  %s\n" "$topic" "$recv" "$payload"
done
