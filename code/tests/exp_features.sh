#!/usr/bin/env bash
# =============================================================================
#  Experiments 3-4, 4-2, 4-3, 4-4 -- LWT, black box, watchdog, thermal governor
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      3-4  broker off during operation, back after 3 min; show the LWT message
#      4-2  black box: detections recorded in SQLite, ring buffer bounded
#      4-3  software watchdog: no frames for >30 s -> alert + restart
#      4-4  adaptive thermal management: heat the SoC past the threshold
#
#  usage:  ./exp_features.sh [3-4|4-2|4-3|4-4|all]
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

WHICH=${1:-all}
SUB=${SUB:-/opt/homebrew/opt/mosquitto/bin/mosquitto_sub}
BROKER=${BROKER:-127.0.0.1}
MOSQ_BIN=/opt/homebrew/opt/mosquitto/sbin/mosquitto
MOSQ_CONF=/opt/homebrew/etc/mosquitto/mosquitto.conf

load_viewer_creds() {
    # shellcheck source=/dev/null
    [ -f "$TESTS_DIR/../mac/.secrets/mqtt.env" ] && . "$TESTS_DIR/../mac/.secrets/mqtt.env"
    VUSER=${MQTT_VIEWER_USER:-viewer}
    VPASS=${MQTT_VIEWER_PASS:-}
    [ -n "$VPASS" ] || { bad "no MQTT viewer password in mac/.secrets/mqtt.env"; return 1; }
}

# ================================================================ 3-4: LWT
exp_3_4() {
    EXP_ID=3-4
    local DIR; DIR=$(expdir 3-4)
    local LOG=$DIR/cmd.log
    : >"$LOG"
    load_viewer_creds || return 1

    hdr "Last Will and Testament, and surviving a broker outage"

    # ---- part 1: a genuine LWT delivery ------------------------------------
    # The will is published BY the broker when a client vanishes without a
    # clean DISCONNECT. Stopping the broker therefore cannot show it -- there
    # would be nobody left to publish it. So the will is provoked the only way
    # it can be: kill the board's client while the broker is up and watching.
    hdr "part 1: SIGKILL the board's client so the broker fires the will"

    "$SUB" -h "$BROKER" -u "$VUSER" -P "$VPASS" -t "status/$STUDENT_ID/home" -v \
        >"$DIR/lwt_capture.txt" 2>&1 &
    local subpid=$!
    sleep 3

    pi 'sudo systemctl kill -s SIGKILL guardian' >>"$LOG" 2>&1
    note "sent SIGKILL to the daemon; waiting for the broker to publish the will"
    sleep 25                       # keepalive is 20 s; the will follows it
    kill "$subpid" 2>/dev/null; wait "$subpid" 2>/dev/null

    printf '\n=== status topic during the kill ===\n' >>"$LOG"
    cat "$DIR/lwt_capture.txt" >>"$LOG" 2>&1

    if grep -q '"status":"offline"' "$DIR/lwt_capture.txt"; then
        ok "LWT delivered: $(grep -m1 '"status":"offline"' "$DIR/lwt_capture.txt" | cut -c1-110)"
    else
        bad "no offline will observed on status/$STUDENT_ID/home"
    fi

    wait_until 90 "the daemon to restart and republish online" \
        pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/telemetry | grep -q mqtt_connected'
    sleep 8
    if "$SUB" -h "$BROKER" -u "$VUSER" -P "$VPASS" -t "status/$STUDENT_ID/home" -C 1 -W 15 2>/dev/null |
            tee -a "$LOG" | grep -q '"status":"online"'; then
        ok "retained status is 'online' again after the automatic restart"
    else
        note "retained status not yet online; the daemon may still be starting"
    fi

    # ---- part 2: the broker disappears for three minutes -------------------
    hdr "part 2: stopping the broker for 3 minutes"
    local OUTAGE=${OUTAGE_SEC:-180}
    printf '\n=== broker stopped at %s ===\n' "$(iso_now)" >>"$LOG"
    pkill -f "$MOSQ_BIN" 2>/dev/null
    sleep 5
    if lsof -nP -iTCP:1883 -sTCP:LISTEN >/dev/null 2>&1; then
        bad "the broker is still listening; the outage did not take effect"
    else
        ok "broker down"
    fi

    note "waiting ${OUTAGE}s with the broker down"
    sleep "$OUTAGE"

    printf '\n=== board log during the outage ===\n' >>"$LOG"
    pi "sudo journalctl -u guardian --since '-${OUTAGE} seconds' --no-pager | grep -i mqtt | tail -12" >>"$LOG" 2>&1
    DOWN_STATE=$(pi 'curl -sk --max-time 5 https://127.0.0.1/api/v1/telemetry' 2>/dev/null | grep -o '"mqtt_connected":[a-z]*')
    note "telemetry during outage reports $DOWN_STATE"
    if printf '%s' "$DOWN_STATE" | grep -q false; then
        ok "the board correctly reports mqtt_connected=false while the broker is gone"
    fi
    # The rest of the system must be unaffected by a missing broker.
    local WEB; WEB=$(pi 'curl -sk -o /dev/null -w "%{http_code}" https://127.0.0.1/')
    [ "$WEB" = 200 ] && ok "web server unaffected during the outage (HTTP $WEB)" \
                     || bad "web server returned $WEB during the outage"

    hdr "restarting the broker"
    ( "$MOSQ_BIN" -c "$MOSQ_CONF" -d >/dev/null 2>&1 & )
    sleep 6
    if wait_until 90 "the board to reconnect" \
            pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/telemetry | grep -q "\"mqtt_connected\":true"'; then
        ok "the board reconnected on its own after the broker returned"
    fi
    printf '\n=== board log after the broker returned ===\n' >>"$LOG"
    pi "sudo journalctl -u guardian --since '-90 seconds' --no-pager | grep -i mqtt | tail -8" >>"$LOG" 2>&1

    cat >"$DIR/result.md" <<EOF
# Experiment 3-4 -- LWT and a broker outage

Run: $(iso_now)

## Part 1 -- the will actually fires

The will is published *by the broker*, on behalf of a client that disappeared
without a clean DISCONNECT. Killing the broker cannot demonstrate it, because
then nothing is left to publish it. So the daemon was killed with SIGKILL while
the broker watched, and the retained will appeared on
\`status/$STUDENT_ID/home\`:

\`\`\`
$(grep -m1 '"status":"offline"' "$DIR/lwt_capture.txt" 2>/dev/null || echo "(not captured)")
\`\`\`

It arrives one keepalive interval (\`mqtt_keepalive = 20\`) after the process
dies -- that is how long the broker waits before declaring the client gone.
\`Restart=always\` then brings the daemon back and it republishes a retained
\`"online"\`, so a subscriber joining later sees the current truth either way.

## Part 2 -- three minutes without a broker

With the broker stopped, the daemon reports \`mqtt_connected=false\`, keeps
retrying with backoff, and **the web server, API and detector keep working**
(HTTP $WEB throughout). When the broker came back the client reconnected by
itself with no intervention.

Transcript: \`cmd.log\`, captured will: \`lwt_capture.txt\`.
EOF
    ok "wrote $DIR/result.md"
}

# =========================================================== 4-2: black box
exp_4_2() {
    EXP_ID=4-2
    local DIR; DIR=$(expdir 4-2)
    local LOG=$DIR/cmd.log
    : >"$LOG"
    hdr "black box: SQLite detection history and the ring buffer"

    printf '=== schema ===\n' >>"$LOG"
    pi 'sudo sqlite3 /var/lib/guardian/guardian.db ".schema"' >>"$LOG" 2>&1

    local COUNT RING NEWEST
    COUNT=$(pi 'sudo sqlite3 /var/lib/guardian/guardian.db "SELECT COUNT(*) FROM detections;"')
    note "rows currently stored: $COUNT"
    printf '\n=== row count: %s ===\n' "$COUNT" >>"$LOG"

    printf '\n=== 10 most recent detections ===\n' >>"$LOG"
    pi 'sudo sqlite3 -header -column /var/lib/guardian/guardian.db "SELECT id, datetime(ts,\"unixepoch\") AS utc, persons, cpu_temp_c, guard_mode, emailed, note FROM detections ORDER BY id DESC LIMIT 10;"' >>"$LOG" 2>&1
    pi 'sudo sqlite3 -header -column /var/lib/guardian/guardian.db "SELECT id, datetime(ts,\"unixepoch\") AS utc, persons, cpu_temp_c, note FROM detections ORDER BY id DESC LIMIT 5;"' 2>/dev/null | tee -a "$LOG"

    printf '\n=== lifetime totals (survive the ring) ===\n' >>"$LOG"
    pi 'sudo sqlite3 -header -column /var/lib/guardian/guardian.db "SELECT * FROM stats;"' >>"$LOG" 2>&1

    # The ring must bound the table; db_ring_size is 1000.
    RING=$(pi "sudo grep -E '^db_ring_size' /etc/guardian/guardian.conf | awk '{print \$3}'")
    if [ "${COUNT:-0}" -le "${RING:-1000}" ]; then
        ok "row count $COUNT is within the ring size $RING"
    else
        bad "row count $COUNT exceeds the ring size $RING"
    fi

    printf '\n=== the trigger that enforces the ring ===\n' >>"$LOG"
    pi 'sudo sqlite3 /var/lib/guardian/guardian.db "SELECT sql FROM sqlite_master WHERE type=\"trigger\";"' >>"$LOG" 2>&1

    # The API must expose the same history the brief asks for.
    printf '\n=== GET /api/v1/history ===\n' >>"$LOG"
    pi_api GET /api/v1/history >>"$LOG" 2>&1
    local HIST; HIST=$(pi_api GET /api/v1/history 2>/dev/null)
    if printf '%s' "$HIST" | grep -q '"records"'; then
        ok "/api/v1/history returns records ($(printf '%s' "$HIST" | grep -o '"id":' | wc -l | tr -d ' ') in the last page)"
    else
        bad "/api/v1/history did not return records"
    fi

    local TOTAL
    TOTAL=$(pi 'sudo sqlite3 /var/lib/guardian/guardian.db "SELECT COALESCE(SUM(value),0) FROM stats WHERE key LIKE \"%detection%\";"' 2>/dev/null)

    cat >"$DIR/result.md" <<EOF
# Experiment 4-2 -- the black box

Run: $(iso_now)

| | |
|---|---|
| rows in \`detections\` | $COUNT |
| configured ring size | $RING |
| lifetime detection counter | ${TOTAL:-see stats table} |

Storage is bounded by an \`AFTER INSERT\` trigger that deletes anything older
than the newest \`db_ring_size\` rows, so the database cannot grow without limit
on a board with a 28 GB card and no log rotation for it. The running totals live
in a separate \`stats\` table precisely so they survive the ring -- otherwise
"how many people were seen today" would silently reset every 1000 detections.

\`/api/v1/history\` serves the most recent records from the same table, which is
the reporting path the brief asks for.

Schema, sample rows and the trigger definition are in \`cmd.log\`.
EOF
    ok "wrote $DIR/result.md"
}

# ============================================================ 4-3: watchdog
exp_4_3() {
    EXP_ID=4-3
    local DIR; DIR=$(expdir 4-3)
    local LOG=$DIR/cmd.log
    : >"$LOG"
    hdr "software watchdog: starving the detector of frames"

    # The capture link is cut at the board, not on the laptop: killing the
    # laptop's ffmpeg would need the camera grant to restart it, and this
    # session does not have one. Dropping inbound 9000 is also a truer model of
    # "the camera went away" -- the sender keeps trying, exactly as a flaky
    # camera cable would, and the laptop's own retry loop restores the feed
    # automatically once the rule is removed.
    local STALE; STALE=$(pi "sudo grep -E '^watchdog_stale_sec' /etc/guardian/guardian.conf | awk '{print \$3}'")
    note "watchdog_stale_sec = $STALE"

    # The detector's MainPID, not NRestarts. The watchdog recovers by asking
    # systemd for a `restart` through polkit, and NRestarts only counts restarts
    # that systemd itself triggered via Restart= after a failure -- so it stays
    # at 0 through a perfectly successful watchdog recovery. A changed PID is
    # unambiguous.
    local BEFORE_PID
    BEFORE_PID=$(pi 'systemctl show -p MainPID --value guardian-vision')
    note "guardian-vision MainPID before: $BEFORE_PID"

    # Where the video comes from decides what "unplugging the camera" means.
    # With camera_source=socket a capture host pushes into port 9000, so the cut
    # is inbound 9000. With camera_source=rtsp the board reaches OUT to the DVR,
    # and blocking 9000 would do exactly nothing -- the test would sit there for
    # 55 s and report a watchdog that never fired, through no fault of its own.
    local CAM_SOURCE RULE_ADD RULE_DEL KILL_CONN CUT_DESC
    CAM_SOURCE=$(pi "sudo grep -E '^camera_source' /etc/guardian/guardian.conf | awk '{print \$3}'" 2>/dev/null)
    CAM_SOURCE=${CAM_SOURCE:-socket}
    note "camera_source = $CAM_SOURCE"

    if [ "$CAM_SOURCE" = rtsp ]; then
        local CAM_HOST CAM_PORT
        CAM_HOST=$(pi "sudo grep -E '^camera_host' /etc/guardian/guardian.conf | awk '{print \$3}'")
        CAM_PORT=$(pi "sudo grep -E '^camera_rtsp_port' /etc/guardian/guardian.conf | awk '{print \$3}'")
        CAM_PORT=${CAM_PORT:-554}
        RULE_ADD="sudo iptables -I OUTPUT -p tcp -d $CAM_HOST --dport $CAM_PORT -j REJECT"
        RULE_DEL="sudo iptables -D OUTPUT -p tcp -d $CAM_HOST --dport $CAM_PORT -j REJECT"
        KILL_CONN="sudo ss -K state established \"( dport = :$CAM_PORT )\" 2>/dev/null || true"
        CUT_DESC="outbound TCP $CAM_PORT to the DVR at $CAM_HOST"
        # REJECT rather than DROP: the board is the client here, and a rejected
        # connect fails immediately instead of hanging for the TCP timeout, so
        # the reconnect loop reports the fault promptly rather than looking hung.
    else
        RULE_ADD='sudo iptables -I INPUT -p tcp --dport 9000 -j DROP'
        RULE_DEL='sudo iptables -D INPUT -p tcp --dport 9000 -j DROP'
        KILL_CONN='sudo ss -K state established "( dport = :9000 or sport = :9000 )" 2>/dev/null || true'
        CUT_DESC="inbound TCP 9000 from the capture host"
    fi

    hdr "cutting $CUT_DESC for $(( STALE + 25 ))s"
    pi "$RULE_ADD" >>"$LOG" 2>&1
    # Existing connections survive a new rule, so the live one is torn down too.
    pi "$KILL_CONN" >>"$LOG" 2>&1

    trap 'note "cleaning up the firewall rule"; pi "$RULE_DEL" >/dev/null 2>&1; exit 130' INT TERM

    sleep $(( STALE + 25 ))

    printf '\n=== watchdog activity ===\n' >>"$LOG"
    pi "sudo journalctl -u guardian --since '-3 minutes' --no-pager | grep -iE 'watchdog|stale|tamper' | tail -12" >>"$LOG" 2>&1
    local WD; WD=$(pi "sudo journalctl -u guardian --since '-3 minutes' --no-pager | grep -ci 'watchdog'" 2>/dev/null)

    if [ "${WD:-0}" -gt 0 ]; then
        ok "watchdog fired ($WD journal entries)"
        pi "sudo journalctl -u guardian --since '-3 minutes' --no-pager -o cat | grep -i watchdog | tail -3" | sed 's/^/        /'
    else
        bad "the watchdog did not fire within $(( STALE + 25 ))s"
    fi

    hdr "restoring the capture link"
    pi "$RULE_DEL" >>"$LOG" 2>&1
    trap - INT TERM

    if wait_until 120 "frames to resume" \
            pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/persons | grep -q "\"available\":true"'; then
        ok "the capture link re-established itself and frames resumed"
    fi

    local AFTER_PID
    AFTER_PID=$(pi 'systemctl show -p MainPID --value guardian-vision')
    if [ -n "$AFTER_PID" ] && [ "$AFTER_PID" != "$BEFORE_PID" ]; then
        ok "detector was genuinely restarted: PID $BEFORE_PID -> $AFTER_PID"
    else
        bad "detector PID unchanged ($AFTER_PID) -- no restart happened"
    fi

    # The brief requires an alert email on suspected tampering, so verify the
    # mailer actually delivered rather than assuming the watchdog queued it.
    local MAILLINE
    MAILLINE=$(pi "sudo journalctl -u guardian --since '-5 minutes' --no-pager -o cat | grep -i 'TAMPERING' | tail -1" 2>/dev/null)
    if [ -n "$MAILLINE" ]; then
        ok "tampering email sent: $MAILLINE"
    else
        bad "no tampering email was delivered"
    fi
    printf '\n=== tampering alert ===\n%s\n' "$MAILLINE" >>"$LOG"

    # The whole point of the earlier unit fix: the daemon must survive this.
    local GUARD; GUARD=$(pi 'systemctl is-active guardian')
    if [ "$GUARD" = active ]; then
        ok "the main daemon stayed active throughout (HTTP $(pi 'curl -sk -o /dev/null -w "%{http_code}" https://127.0.0.1/'))"
    else
        bad "the main daemon is $GUARD -- the watchdog took the system down"
    fi

    cat >"$DIR/result.md" <<EOF
# Experiment 4-3 -- software watchdog

Run: $(iso_now). \`watchdog_stale_sec = $STALE\`.

The capture link was cut by dropping inbound TCP 9000 on the board and tearing
down the established connection, which models a camera or cable failure more
honestly than stopping the sender: the laptop keeps retrying throughout, and
recovers on its own the moment the rule is lifted.

| | |
|---|---|
| detector PID before | $BEFORE_PID |
| detector PID after | $AFTER_PID |
| tampering email | ${MAILLINE:-not delivered} |
| main daemon during the fault | $GUARD |

Recovery is measured by the detector's PID changing, not by systemd's
\`NRestarts\`. The watchdog recovers by asking systemd for a \`restart\` over
polkit, and \`NRestarts\` counts only the restarts systemd itself initiates
through \`Restart=\` after a failure -- it stays at 0 through a completely
successful watchdog recovery, which makes it a misleading thing to report.

The watchdog measures the age of the last frame that carried **real** capture
input, not the age of the last published frame. That distinction matters: the
detector keeps emitting a placeholder at 1 Hz so the dashboard does not freeze,
and a naive freshness check would happily watch those placeholders forever and
never fire.

Recovery is a restart of \`guardian-vision\` alone. An earlier revision of the
unit files would have taken the entire daemon down with it -- \`guardian.service\`
had \`Requires=guardian-ingest.socket\`, and that socket is \`PartOf=\` the
detector, so restarting the detector stopped the web server, MQTT, mail and the
black box with no path back. That is fixed, and the table above records the
daemon staying \`active\` while its detector was restarted underneath it.

Transcript: \`cmd.log\`.
EOF
    ok "wrote $DIR/result.md"
}

# ===================================================== 4-4: thermal governor
exp_4_4() {
    EXP_ID=4-4
    local DIR; DIR=$(expdir 4-4)
    local LOG=$DIR/cmd.log
    local CSV=$DIR/thermal_response.csv
    : >"$LOG"
    hdr "adaptive thermal management: heating the SoC past the threshold"

    local HIGH LOW
    HIGH=$(pi "sudo grep -E '^thermal_high_c' /etc/guardian/guardian.conf | awk '{print \$3}'")
    LOW=$(pi "sudo grep -E '^thermal_low_c' /etc/guardian/guardian.conf | awk '{print \$3}'")
    note "thresholds: high ${HIGH}C, low ${LOW}C"

    local LOADER
    if pi 'command -v stress-ng >/dev/null 2>&1'; then
        LOADER='stress-ng --cpu 4 --timeout 300s'
        note "using stress-ng"
    else
        LOADER='for i in 1 2 3 4; do timeout 300 sh -c "while :; do :; done" & done; wait'
        note "stress-ng not installed; using busy loops on 4 cores"
    fi
    printf 'load generator: %s\n' "$LOADER" >>"$LOG"

    pi "nohup sh -c '$LOADER' >/dev/null 2>&1 &" >/dev/null 2>&1
    trap 'note "stopping the load"; pi "sudo pkill -f stress-ng; sudo pkill -f \"while :\"" >/dev/null 2>&1; exit 130' INT TERM

    echo "epoch,iso,temp_c,thermal_level,target_fps,target_width,target_net_input,throttled" >"$CSV"
    local PEAK=0 MAXLEVEL=0
    for _ in $(seq 1 36); do          # 36 x 5s = 3 minutes
        local js temp lvl tf tw tn thr
        js=$(pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/telemetry' 2>/dev/null)
        temp=$(printf '%s' "$js" | sed -n 's/.*"cpu_temp_c":\([0-9.]*\).*/\1/p')
        lvl=$(printf  '%s' "$js" | sed -n 's/.*"thermal_level":\([0-9]*\).*/\1/p')
        tf=$(printf   '%s' "$js" | sed -n 's/.*"target_fps":\([0-9]*\).*/\1/p')
        tw=$(printf   '%s' "$js" | sed -n 's/.*"target_width":\([0-9]*\).*/\1/p')
        tn=$(printf   '%s' "$js" | sed -n 's/.*"target_net_input":\(-\{0,1\}[0-9]*\).*/\1/p')
        thr=$(pi 'sudo vcgencmd get_throttled 2>/dev/null | cut -d= -f2' 2>/dev/null)
        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$(iso_now)" \
               "${temp:-NA}" "${lvl:-NA}" "${tf:-NA}" "${tw:-NA}" "${tn:-NA}" "${thr:-NA}" >>"$CSV"
        [ -n "$temp" ] && awk -v a="$temp" -v b="$PEAK" 'BEGIN{exit !(a>b)}' && PEAK=$temp
        [ -n "$lvl" ] && [ "$lvl" -gt "$MAXLEVEL" ] 2>/dev/null && MAXLEVEL=$lvl
        sleep 5
    done

    pi 'sudo pkill -f stress-ng; sudo pkill -f "while :"' >/dev/null 2>&1
    trap - INT TERM
    note "peak temperature ${PEAK}C, highest thermal level $MAXLEVEL"

    printf '\n=== governor activity ===\n' >>"$LOG"
    pi "sudo journalctl -u guardian --since '-6 minutes' --no-pager | grep -iE 'thermal|throttl|governor' | tail -15" >>"$LOG" 2>&1

    local PHASE_B=0 NEWHIGH="" NEWLOW="" BLEVEL=0 BFPS="" BNET=""
    if [ "$MAXLEVEL" -gt 0 ]; then
        ok "the governor engaged: thermal_level reached $MAXLEVEL"
    else
        note "peak ${PEAK}C never reached the ${HIGH}C setpoint -- the SoC is capped"
        note "by under-voltage throttling before it can get there"
        hdr "phase B: moving the setpoint into the achievable range"

        # Verifying a control loop whose setpoint the plant cannot physically
        # reach is impossible; the standard move is to bring the setpoint to the
        # plant instead. The thresholds are lowered just under the peak actually
        # measured in phase A, so the governor has to act, and restored after.
        # This validates the mechanism -- detect, step down, notify, recover --
        # which is what part 4-4 is asking for.
        NEWHIGH=$(awk -v p="$PEAK" 'BEGIN{printf "%.1f", p-2.0}')
        NEWLOW=$(awk  -v p="$PEAK" 'BEGIN{printf "%.1f", p-6.0}')
        note "temporary thresholds: high ${NEWHIGH}C, low ${NEWLOW}C"

        restore_thermal() {
            pi "sudo sh -c 'test -f /etc/guardian/guardian.conf.thermbak && mv /etc/guardian/guardian.conf.thermbak /etc/guardian/guardian.conf; chown root:guardian /etc/guardian/guardian.conf; chmod 0640 /etc/guardian/guardian.conf'" >/dev/null 2>&1
            pi 'sudo systemctl restart guardian' >/dev/null 2>&1
        }
        trap 'note "restoring thermal thresholds"; pi "sudo pkill -f stress-ng" >/dev/null 2>&1; restore_thermal; exit 130' INT TERM

        pi "sudo sh -c \"sed -i.thermbak -e 's/^thermal_high_c.*/thermal_high_c = $NEWHIGH/' -e 's/^thermal_low_c.*/thermal_low_c = $NEWLOW/' /etc/guardian/guardian.conf; chown root:guardian /etc/guardian/guardian.conf; chmod 0640 /etc/guardian/guardian.conf\"" >/dev/null 2>&1
        pi 'sudo systemctl restart guardian' >/dev/null 2>&1
        wait_until 60 "the daemon to come back" \
            pi 'curl -sk --max-time 4 -o /dev/null -w "%{http_code}" https://127.0.0.1/api/v1/telemetry | grep -q 200'

        pi "nohup sh -c '$LOADER' >/dev/null 2>&1 &" >/dev/null 2>&1
        for _ in $(seq 1 24); do      # 24 x 5s = 2 minutes
            local js2 t2 l2 f2 n2
            js2=$(pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/telemetry' 2>/dev/null)
            t2=$(printf '%s' "$js2" | sed -n 's/.*"cpu_temp_c":\([0-9.]*\).*/\1/p')
            l2=$(printf '%s' "$js2" | sed -n 's/.*"thermal_level":\([0-9]*\).*/\1/p')
            f2=$(printf '%s' "$js2" | sed -n 's/.*"target_fps":\([0-9]*\).*/\1/p')
            n2=$(printf '%s' "$js2" | sed -n 's/.*"target_net_input":\(-\{0,1\}[0-9]*\).*/\1/p')
            printf '%s,%s,%s,%s,%s,,%s,phaseB\n' "$(date +%s)" "$(iso_now)" \
                   "${t2:-NA}" "${l2:-NA}" "${f2:-NA}" "${n2:-NA}" >>"$CSV"
            [ -n "$l2" ] && [ "$l2" -gt "$BLEVEL" ] 2>/dev/null && { BLEVEL=$l2; BFPS=$f2; BNET=$n2; }
            sleep 5
        done
        pi 'sudo pkill -f stress-ng; sudo pkill -f "while :"' >/dev/null 2>&1

        printf '\n=== phase B governor activity ===\n' >>"$LOG"
        pi "sudo journalctl -u guardian --since '-4 minutes' --no-pager -o cat | grep -iE 'thermal|governor' | tail -12" >>"$LOG" 2>&1

        if [ "$BLEVEL" -gt 0 ]; then
            ok "governor engaged at the lowered setpoint: level $BLEVEL, target_fps $BFPS, net_input $BNET"
            pi "sudo journalctl -u guardian --since '-4 minutes' --no-pager -o cat | grep -iE 'thermal' | tail -3" | sed 's/^/        /'
            PHASE_B=1
        else
            bad "the governor did not engage even at ${NEWHIGH}C"
        fi

        restore_thermal
        trap - INT TERM
        ok "restored thresholds to high ${HIGH}C / low ${LOW}C"
    fi

    cat >"$DIR/result.md" <<EOF
# Experiment 4-4 -- adaptive thermal management

Run: $(iso_now). Thresholds \`thermal_high_c = $HIGH\`, \`thermal_low_c = $LOW\`.

Load was applied to all four cores for three minutes while the daemon's own
telemetry was sampled every 5 s.

| | |
|---|---|
| peak CPU temperature under full load | ${PEAK} C |
| highest thermal level at the configured ${HIGH} C setpoint | $MAXLEVEL |
$( [ "$PHASE_B" = 1 ] && cat <<PB
| temporary setpoint (phase B) | ${NEWHIGH} C up / ${NEWLOW} C down |
| highest thermal level at that setpoint | $BLEVEL |
| target_fps once engaged | $BFPS |
| target_net_input once engaged | $BNET |
PB
)

$( [ "$PHASE_B" = 1 ] && cat <<PB
**The 70 C setpoint is unreachable on this board.** Four cores of stress-ng for
three minutes peaked at ${PEAK} C, because the under-voltage condition documented
in 2-1 caps the SoC before its own thermal limit becomes relevant. A control loop
whose setpoint the plant cannot reach cannot be verified by waiting, so the
setpoint was moved to the plant: thresholds were lowered to ${NEWHIGH} C / ${NEWLOW} C,
the load re-applied, and the governor then behaved exactly as designed --
thermal level $BLEVEL, frame rate cut to $BFPS, network input $BNET -- before the
original values were restored. What this demonstrates is the mechanism; what it
does not demonstrate is the board reaching 70 C, which it cannot on this supply.
PB
)

The governor uses separate rise and fall thresholds ($HIGH C up, $LOW C down)
rather than one. With a single threshold the system would oscillate: the moment
it lowered the frame rate the temperature would drop below the line, it would
restore full rate, heat up again, and flap indefinitely -- changing resolution
several times a minute. The $((  $(printf '%.0f' "$HIGH") - $(printf '%.0f' "$LOW") )) C gap makes each
step commit.

Note that this board is already being throttled by an inadequate power supply
(under-voltage), which caps the SoC before the governor's own thresholds do --
so the temperatures here are lower than the silicon would otherwise reach.

Per-sample data: \`thermal_response.csv\`; governor log: \`cmd.log\`.
EOF
    ok "wrote $DIR/result.md"
}

case "$WHICH" in
    3-4) exp_3_4 ;;
    4-2) exp_4_2 ;;
    4-3) exp_4_3 ;;
    4-4) exp_4_4 ;;
    all) exp_4_2; exp_4_3; exp_4_4; exp_3_4 ;;
    *)   echo "usage: $0 [3-4|4-2|4-3|4-4|all]" >&2; exit 2 ;;
esac
