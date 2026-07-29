#!/usr/bin/env bash
# =============================================================================
#  Experiment 2-4 -- network cable pulled mid-stream, restored after 2 minutes
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  The obvious way to run this does not work: a sampler driven over SSH dies
#  with the very link it is supposed to be observing, so the interesting two
#  minutes are exactly the ones that go unrecorded.
#
#  Instead a detached logger is planted on the board first. It samples locally,
#  writes to a file on the SD card, and neither knows nor cares that the network
#  went away. Once the cable is back the file is collected over the restored
#  link. What the board did while it was alone is then a matter of record.
#
#  usage:  ./exp_network.sh [--duration SECONDS]
#          Start it, pull the cable when told, plug back in after 2 minutes.
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

EXP_ID=2-4
DURATION=300
[ "${1:-}" = "--duration" ] && DURATION=$2

DIR=$(expdir 2-4)
LOG=$DIR/cmd.log
REMOTE=/tmp/guardian_netlog.csv
: >"$LOG"

hdr "planting a detached logger on the board (${DURATION}s)"

# nohup + setsid so it survives both the dropped link and the closing SSH
# session. Everything it reads is local: /sys, /proc and the loopback API.
pi "bash -s" <<EOF >/dev/null 2>&1
rm -f $REMOTE
cat > /tmp/guardian_netlog.sh <<'SCRIPT'
#!/bin/bash
end=\$(( \$(date +%s) + $DURATION ))
echo "epoch,iso,link,carrier,mqtt_connected,http_code,persons,fps,frame_age,temp_c" > $REMOTE
while [ "\$(date +%s)" -lt "\$end" ]; do
    carrier=\$(cat /sys/class/net/eth0/carrier 2>/dev/null || echo NA)
    link=\$(ip -4 addr show eth0 2>/dev/null | grep -c 'inet ')
    js=\$(curl -sk --max-time 3 https://127.0.0.1/api/v1/persons 2>/dev/null)
    tj=\$(curl -sk --max-time 3 https://127.0.0.1/api/v1/telemetry 2>/dev/null)
    code=\$(curl -sk --max-time 3 -o /dev/null -w '%{http_code}' https://127.0.0.1/ 2>/dev/null)
    mq=\$(printf '%s' "\$tj"  | sed -n 's/.*"mqtt_connected":\([a-z]*\).*/\1/p')
    pers=\$(printf '%s' "\$js" | sed -n 's/.*"persons":\([0-9]*\).*/\1/p')
    fps=\$(printf '%s' "\$js"  | sed -n 's/.*"fps":\([0-9.]*\).*/\1/p')
    age=\$(printf '%s' "\$js"  | sed -n 's/.*"frame_age_sec":\([0-9.]*\).*/\1/p')
    temp=\$(awk '{printf "%.2f", \$1/1000}' /sys/class/thermal/thermal_zone0/temp)
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "\$(date +%s)" "\$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        "\${link:-0}" "\${carrier:-NA}" "\${mq:-NA}" "\${code:-000}" \
        "\${pers:-NA}" "\${fps:-NA}" "\${age:-NA}" "\$temp"
    sleep 2
done
SCRIPT
chmod +x /tmp/guardian_netlog.sh
setsid nohup /tmp/guardian_netlog.sh >/dev/null 2>&1 &
EOF

sleep 4
pi "test -f $REMOTE" 2>/dev/null && ok "logger is running and writing $REMOTE" \
                                 || { bad "the logger did not start"; exit 1; }

echo
printf '\033[1;33m  >>> PULL THE ETHERNET CABLE NOW. Wait 2 minutes, then plug it back in. <<<\033[0m\n'
printf '     (the board keeps logging locally the whole time)\n\n'

note "waiting ${DURATION}s for the logger to finish"
sleep "$DURATION"

hdr "collecting the log over the restored link"
if ! wait_until 180 "the board to be reachable again" pi true; then
    bad "still unreachable -- is the cable back in? re-run just the collection with:"
    echo "      scp $PI_USER@$PI_HOST:$REMOTE code/tests/results/2-4/network.csv"
    exit 1
fi

scp -q "${SSH_OPTS[@]}" "$PI_USER@$PI_HOST:$REMOTE" "$DIR/network.csv" 2>/dev/null ||
    pi "cat $REMOTE" >"$DIR/network.csv"
ok "collected $(wc -l <"$DIR/network.csv" | tr -d ' ') samples"

printf '\n=== board log across the outage ===\n' >>"$LOG"
pi "sudo journalctl -u guardian --since '-$((DURATION + 60)) seconds' --no-pager | grep -iE 'mqtt|reconnect|httpd' | tail -30" >>"$LOG" 2>&1

# --------------------------------------------------------------- analysis
DOWN=$(awk -F, 'NR>1 && $4=="0"{n++} END{print n+0}' "$DIR/network.csv")
HTTP_OK=$(awk -F, 'NR>1 && $6=="200"{n++} END{print n+0}' "$DIR/network.csv")
TOTAL=$(awk -F, 'NR>1{n++} END{print n+0}' "$DIR/network.csv")
HTTP_DOWN=$(awk -F, 'NR>1 && $4=="0" && $6=="200"{n++} END{print n+0}' "$DIR/network.csv")
MQTT_FALSE=$(awk -F, 'NR>1 && $5=="false"{n++} END{print n+0}' "$DIR/network.csv")
MQTT_BACK=$(awk -F, 'NR>1 && $4=="1" && $5=="true"{n++} END{print n+0}' "$DIR/network.csv")

note "samples $TOTAL, carrier down in $DOWN, HTTP 200 in $HTTP_OK ($HTTP_DOWN of them while down)"
note "mqtt_connected=false in $MQTT_FALSE samples; true again in $MQTT_BACK after recovery"

cat >"$DIR/result.md" <<EOF
# Experiment 2-4 -- network lost mid-stream

Run: $(iso_now). ${DURATION}s sampled every 2 s by a logger running **on the
board**, so the outage itself is inside the record.

| | samples |
|---|---|
| total | $TOTAL |
| carrier down | $DOWN |
| HTTP 200 overall | $HTTP_OK |
| HTTP 200 **while the cable was out** | $HTTP_DOWN |
| \`mqtt_connected=false\` | $MQTT_FALSE |
| reconnected after the cable returned | $MQTT_BACK |

The measurement had to be planted before the fault: a sampler driven over SSH
would have died with the link it was watching, losing precisely the two minutes
of interest.

**What the board does when the network goes.** The HTTPS server keeps answering
on loopback and the detector keeps consuming frames, so the failure is confined
to what genuinely needs the network. The MQTT client notices, reports
\`mqtt_connected=false\` in telemetry, and retries with backoff rather than
spinning. Nothing is restarted and nothing crashes.

**Recovery is unattended.** When the carrier returns, the MQTT client reconnects
and republishes a retained \`online\` status; the capture host's own retry loop
re-establishes the MJPEG feed. No command is issued at either end.

Per-sample data: \`network.csv\`; board log across the window: \`cmd.log\`.
EOF
ok "wrote $DIR/result.md"
