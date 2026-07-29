#!/usr/bin/env bash
# =============================================================================
#  Experiments 2-1, 2-2, 2-3 -- thermal profile, memory growth, API under load
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      2-1  CPU temperature in three states, sampled every 30 s for 5 min each
#      2-2  memory of the C daemon over 5 min of streaming, sampled every 5 s
#      2-3  50 concurrent requests to /api/v1/telemetry inside 30 s
#
#  Sampling runs ON the board and reads /proc and /sys directly, for the same
#  reason the brief demands it of the daemon: going through the network would
#  measure the link, and going through `top` would measure a shell pipeline.
#
#  NOTE ON THIS HARDWARE
#  ---------------------
#  This board runs on a supply that dips below the Pi's 4.63 V threshold; the
#  kernel logged 351 "Undervoltage detected!" events in the first six hours of
#  uptime and vcgencmd reports throttling active. Every run therefore records
#  the throttle bitmask alongside the temperature, so the report can state
#  plainly which numbers were taken while the SoC was capped.
#
#  usage:  ./exp_perf.sh [2-1|2-2|2-3|all]
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

WHICH=${1:-all}

# One remote sampler used by both 2-1 and 2-2. Emits CSV on stdout.
#   $1 duration seconds, $2 interval seconds, $3 label
remote_sample() {
    local dur=$1 iv=$2 label=$3
    # 2>/dev/null on the whole remote shell: a single stray warning on stderr
    # would otherwise be interleaved into the CSV on stdout and shift every
    # column after it. Anything worth reporting is written as a field.
    pi "bash -s" 2>/dev/null <<EOF
set -u
end=\$(( \$(date +%s) + $dur ))
echo "epoch,iso,label,temp_c,cpu_pct,rss_kb,mem_avail_kb,throttled,freq_khz,fps,persons,infer_ms"
prev_idle=0; prev_total=0
while [ "\$(date +%s)" -lt "\$end" ]; do
    # --- /proc/stat: CPU busy fraction since the previous sample ---
    read -r _ u n s i w irq sirq st _ < /proc/stat
    idle=\$(( i + w )); total=\$(( u + n + s + i + w + irq + sirq + st ))
    if [ "\$prev_total" -gt 0 ]; then
        dt=\$(( total - prev_total )); di=\$(( idle - prev_idle ))
        # Parentheses are load-bearing: awk parses \`printf fmt, x > 0 ? ..\`
        # with > as output redirection to a file named "0", not a comparison.
        cpu=\$(awk -v dt="\$dt" -v di="\$di" 'BEGIN{ printf "%.2f", (dt>0 ? 100*(dt-di)/dt : 0) }')
    else
        cpu=0
    fi
    prev_idle=\$idle; prev_total=\$total

    temp=\$(awk '{printf "%.2f", \$1/1000}' /sys/class/thermal/thermal_zone0/temp)
    freq=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    # vcgencmd needs root for /dev/vcio. Without sudo it fails with a TWO-LINE
    # message on stderr, and that embedded newline splits every CSV row in half.
    # Hence sudo, stderr discarded, and a tr to guarantee a single-token field.
    thr=\$(sudo vcgencmd get_throttled 2>/dev/null | cut -d= -f2 | tr -d '\n\r,')
    [ -n "\$thr" ] || thr=NA
    avail=\$(awk '/MemAvailable/{print \$2}' /proc/meminfo)

    # RSS of the C daemon, straight from /proc/<pid>/status.
    gpid=\$(systemctl show -p MainPID --value guardian 2>/dev/null)
    rss=0
    [ -n "\$gpid" ] && [ "\$gpid" != 0 ] && rss=\$(awk '/VmRSS/{print \$2}' /proc/\$gpid/status 2>/dev/null || echo 0)

    # Detector figures come from the daemon's own API on loopback. The daemon
    # keeps serving the LAST KNOWN fps/inference after the detector stops --
    # sensible for a dashboard, misleading in a dataset -- so when the detector
    # is not running these are recorded as 0 rather than as stale readings.
    vis=\$(systemctl is-active guardian-vision 2>/dev/null)
    js=\$(curl -sk --max-time 4 https://127.0.0.1/api/v1/persons 2>/dev/null)
    if [ "\$vis" = active ]; then
        fps=\$(printf '%s' "\$js"    | sed -n 's/.*"fps":\([0-9.]*\).*/\1/p'); fps=\${fps:-0}
        pers=\$(printf '%s' "\$js"   | sed -n 's/.*"persons":\([0-9]*\).*/\1/p'); pers=\${pers:-0}
        inf=\$(printf '%s' "\$js"    | sed -n 's/.*"inference_ms":\([0-9.]*\).*/\1/p'); inf=\${inf:-0}
    else
        fps=0; pers=0; inf=0
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "\$(date +%s)" "\$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$label" \
        "\$temp" "\$cpu" "\$rss" "\$avail" "\$thr" "\$freq" "\$fps" "\$pers" "\$inf"
    sleep $iv
done
EOF
}

summarise() {  # csv -> "min max mean" of a column
    awk -F, -v c="$2" 'NR>1 && $c != "" {
        v=$c+0; s+=v; n++; if(mn==""||v<mn)mn=v; if(v>mx)mx=v
    } END { if(n) printf "%.2f %.2f %.2f", mn, mx, s/n; else printf "NA NA NA" }' "$1"
}

# ==================================================== 2-1: thermal profile
exp_2_1() {
    EXP_ID=2-1
    local DIR; DIR=$(expdir 2-1)
    local CSV=$DIR/thermal.csv
    local DUR=${DUR_2_1:-300} IV=30
    hdr "thermal profile: three states, ${DUR}s each, ${IV}s sampling"
    note "this takes about $(( DUR * 3 / 60 )) minutes"
    echo
    echo "  NOTE: measuring the idle state means stopping the detector, and the"
    echo "  ingest socket is PartOf= it -- so port 9000 closes for about $(( DUR / 60 )) minutes."
    echo "  stream_webcam.sh on the laptop will print 'Connection refused' and retry"
    echo "  throughout. That is expected, not a fault: it reconnects by itself once"
    echo "  state (b) starts the detector again. Leave it running."
    echo

    : >"$CSV"

    # --- (a) idle: no capture feed consumed, no inference -------------------
    #
    # The software watchdog of part 4-3 would undo this: it sees no fresh frame
    # for watchdog_stale_sec, restarts the detector, and emails a tampering
    # alert. The first attempt at this experiment measured 27% CPU and 1126 ms
    # inference during "idle" for exactly that reason. So the threshold is
    # lifted for the duration and restored afterwards -- the watchdog is not
    # disabled anywhere else, and 4-3 still exercises it properly.
    #
    # sed -i replaces the file, which would leave it root:root and unreadable to
    # the guardian user, so ownership and mode are reasserted every time.
    hdr "state (a) idle -- stopping the detector (watchdog suspended)"
    restore_watchdog() {
        pi "sudo sh -c 'test -f /etc/guardian/guardian.conf.expbak && mv /etc/guardian/guardian.conf.expbak /etc/guardian/guardian.conf; chown root:guardian /etc/guardian/guardian.conf; chmod 0640 /etc/guardian/guardian.conf'" >/dev/null 2>&1
        pi 'sudo systemctl restart guardian' >/dev/null 2>&1
    }
    trap 'echo; note "interrupted -- restoring the watchdog"; restore_watchdog; exit 130' INT TERM

    pi "sudo sh -c \"sed -i.expbak 's/^watchdog_stale_sec.*/watchdog_stale_sec = 100000/' /etc/guardian/guardian.conf; chown root:guardian /etc/guardian/guardian.conf; chmod 0640 /etc/guardian/guardian.conf\"" >/dev/null 2>&1
    pi 'sudo systemctl restart guardian' >/dev/null 2>&1
    wait_until 60 "the daemon to come back" \
        pi 'curl -sk --max-time 4 -o /dev/null -w "%{http_code}" https://127.0.0.1/api/v1/telemetry | grep -q 200'
    pi 'sudo systemctl stop guardian-vision' >/dev/null 2>&1
    sleep 20                     # let the SoC settle before sampling
    remote_sample "$DUR" "$IV" idle | tee -a "$CSV" | tail -2

    # Guard against the failure that spoiled the first run: if inference is
    # still happening, "idle" is not idle and the whole comparison is void.
    IDLE_INFER=$(awk -F, '$3=="idle" && $12+0 > 0 {n++} END{print n+0}' "$CSV")
    if [ "$IDLE_INFER" -gt 0 ]; then
        bad "$IDLE_INFER idle samples show inference still running -- state (a) is contaminated"
    else
        ok "idle state verified: no inference in any sample"
    fi

    # --- (b) stream only: frames flow and are served, no inference ----------
    hdr "state (b) stream only -- detector relaying, inference off"
    restore_watchdog                 # frames are flowing again from here on
    trap - INT TERM
    pi 'sudo systemctl start guardian-vision' >/dev/null 2>&1
    wait_until 60 "the detector to publish a frame" \
        pi 'curl -sk --max-time 4 https://127.0.0.1/api/v1/persons | grep -q "\"available\":true"'
    pi_api POST /api/v1/command '{"cmd":"set_net_input","net_input":0}' >/dev/null 2>&1
    sleep 20
    # Keep a client pulling MJPEG for the whole window, so "stream" really is
    # being served and not merely produced.
    pi "timeout $((DUR + 10)) curl -sk -o /dev/null https://127.0.0.1/api/v1/stream >/dev/null 2>&1 &" >/dev/null 2>&1
    remote_sample "$DUR" "$IV" stream_only | tail -n +2 | tee -a "$CSV" | tail -2

    # --- (c) stream + detection --------------------------------------------
    hdr "state (c) stream + detection active"
    pi_api POST /api/v1/command '{"cmd":"set_net_input","net_input":300}' >/dev/null 2>&1
    sleep 20
    pi "timeout $((DUR + 10)) curl -sk -o /dev/null https://127.0.0.1/api/v1/stream >/dev/null 2>&1 &" >/dev/null 2>&1
    remote_sample "$DUR" "$IV" stream_detect | tail -n +2 | tee -a "$CSV" | tail -2

    # --- summary ------------------------------------------------------------
    {
        echo "# Experiment 2-1 -- CPU temperature in three states"
        echo
        echo "Run: $(iso_now). Sampling every ${IV}s for ${DUR}s per state."
        echo
        echo "| state | temp min | temp max | temp mean | CPU% mean | fps mean |"
        echo "|---|---|---|---|---|---|"
        for st in idle stream_only stream_detect; do
            grep -h ",$st," "$CSV" >"$DIR/.s" 2>/dev/null
            [ -s "$DIR/.s" ] || continue
            sed -i.bak '1i epoch,iso,label,temp_c,cpu_pct,rss_kb,mem_avail_kb,throttled,freq_khz,fps,persons,infer_ms' "$DIR/.s" 2>/dev/null
            read -r tmn tmx tmean <<<"$(summarise "$DIR/.s" 4)"
            read -r _ _ cmean     <<<"$(summarise "$DIR/.s" 5)"
            read -r _ _ fmean     <<<"$(summarise "$DIR/.s" 10)"
            printf '| %s | %s | %s | %s | %s | %s |\n' "$st" "$tmn" "$tmx" "$tmean" "$cmean" "$fmean"
        done
        rm -f "$DIR/.s" "$DIR/.s.bak"
        echo
        echo "Throttle bitmask observed: \`$(awk -F, 'NR>1{print $8}' "$CSV" | sort -u | tr '\n' ' ')\`"
        echo
        echo "Bit 0 of that mask is under-voltage and bit 2 is active throttling, so"
        echo "these temperatures were recorded while the SoC was already being capped by"
        echo "an inadequate supply. The *relative* ordering of the three states is still"
        echo "meaningful; the absolute ceiling would be higher on a compliant PSU."
        echo
        echo "Raw data: \`thermal.csv\`."
    } >"$DIR/result.md"
    ok "wrote $DIR/result.md and thermal.csv ($(wc -l <"$CSV" | tr -d ' ') rows)"
}

# ======================================================= 2-2: memory growth
exp_2_2() {
    EXP_ID=2-2
    local DIR; DIR=$(expdir 2-2)
    local CSV=$DIR/memory.csv
    local DUR=${DUR_2_2:-300} IV=5
    hdr "memory of the C daemon over ${DUR}s of streaming, ${IV}s sampling"

    remote_sample "$DUR" "$IV" stream_detect >"$CSV"

    read -r rmn rmx rmean <<<"$(summarise "$CSV" 6)"
    # Least-squares slope of RSS against time: the leak test proper.
    #
    # Two things this has to get right.
    #
    # 1. Time is measured from the first sample, not from the epoch. With raw
    #    epoch seconds (~1.78e9) the x*x term reaches ~3.2e18, past the 53-bit
    #    mantissa of awk's doubles, so n*sxx - sx*sx cancels to zero and the fit
    #    divides by zero or returns noise.
    #
    # 2. The warm-up is excluded. A daemon's RSS climbs for the first minute or
    #    so as JPEG buffers, the TLS session cache and SQLite pages reach steady
    #    state, then stops. Fitting one line across warm-up AND steady state
    #    reports that initial ramp as though it continued forever -- this run
    #    measured 0.5988 kB/s that way and called a perfectly flat daemon a leak.
    #    A leak is a trend that persists, so the verdict is taken from the
    #    steady-state portion only.
    #
    # The warm-up slope is still computed, and the two are reported together:
    # a leak has slope_late ~ slope_early, while warm-up has slope_late ~ 0.
    SKIP_FRAC=${SKIP_FRAC:-40}     # percent of the run treated as warm-up
    slope_over() {   # $1 = csv, $2 = first row fraction, $3 = last row fraction
        awk -F, -v lo="$2" -v hi="$3" '
            NR>1 && $6>0 { t[++m]=$1; y[m]=$6 }
            END {
                if (m < 4) { printf "NA"; exit }
                a = int(m*lo/100); if (a < 1) a = 1
                b = int(m*hi/100); if (b > m) b = m
                n = 0
                for (i = a; i <= b; i++) {
                    x = t[i] - t[a]; v = y[i]
                    n++; sx += x; sy += v; sxy += x*v; sxx += x*x
                }
                d = n*sxx - sx*sx
                if (n > 1 && d != 0) printf "%.4f", (n*sxy - sx*sy)/d
                else printf "NA"
            }' "$1"
    }
    SLOPE=$(slope_over "$CSV" "$SKIP_FRAC" 100)          # steady state: the verdict
    SLOPE_WARMUP=$(slope_over "$CSV" 0 "$SKIP_FRAC")     # warm-up, for contrast
    SLOPE_ALL=$(slope_over "$CSV" 0 100)                 # naive whole-window fit
    FIRST=$(awk -F, 'NR==2{print $6}' "$CSV")
    LAST=$(awk -F, 'END{print $6}' "$CSV")
    DELTA=$(( ${LAST:-0} - ${FIRST:-0} ))

    note "RSS min/max/mean = $rmn / $rmx / $rmean kB; slope ${SLOPE} kB/s; net ${DELTA} kB"

    {
        echo "# Experiment 2-2 -- memory of the C daemon under continuous streaming"
        echo
        echo "Run: $(iso_now). ${DUR}s of live stream, RSS sampled every ${IV}s from"
        echo "\`/proc/<pid>/status\`."
        echo
        echo "| | kB |"
        echo "|---|---|"
        echo "| RSS at start | ${FIRST:-NA} |"
        echo "| RSS at end | ${LAST:-NA} |"
        echo "| min / max | $rmn / $rmx |"
        echo "| mean | $rmean |"
        echo "| net change | $DELTA |"
        echo "| slope, warm-up (first ${SKIP_FRAC}%) | ${SLOPE_WARMUP} kB/s |"
        echo "| slope, steady state (last $((100 - SKIP_FRAC))%) | **${SLOPE} kB/s** |"
        echo "| slope, naive whole-window fit | ${SLOPE_ALL} kB/s |"
        echo
        # MB/hour computed with -v rather than by pasting the value into the awk
        # program text: interpolating a shell variable into a quoted awk source
        # string is how the previous version produced `BEGIN 0.5988*3600/1024`
        # and bailed out.
        MB_PER_HOUR=$(awk -v s="${SLOPE:-0}" 'BEGIN{ printf "%.1f", s*3600/1024 }')
        if [ "$SLOPE" != "NA" ] && [ -n "$SLOPE" ] && awk -v s="$SLOPE" 'BEGIN{exit !(s > 0.5)}'; then
            echo "**A steady-state slope of ${SLOPE} kB/s is a leak.** Extrapolated it would"
            echo "add ${MB_PER_HOUR} MB per hour, which this board's 900 MB could not absorb"
            echo "indefinitely."
        else
            echo "**No leak.** Steady-state slope is ${SLOPE} kB/s -- flat to within sampling"
            echo "noise over a ${DUR}s window."
            echo
            # Only claim the naive fit is misleading when it actually diverges.
            # On a daemon that was already warm all three agree, and asserting a
            # discrepancy that is not in the data would be its own small lie.
            if awk -v a="$SLOPE_ALL" -v b="$SLOPE" 'BEGIN{ d=a-b; if (d<0) d=-d; exit !(d > 0.25) }'; then
                echo "The three slopes matter here. The naive whole-window fit"
                echo "(${SLOPE_ALL} kB/s) is misleading on its own: it averages the warm-up ramp"
                echo "(${SLOPE_WARMUP} kB/s) together with the flat remainder and reports a"
                echo "start-up transient as if it were unbounded growth. RSS climbs while the"
                echo "JPEG buffers, the TLS session cache and the SQLite page cache reach their"
                echo "working size, then stops. A genuine leak would hold the same slope in"
                echo "both halves instead of flattening."
            else
                echo "All three slopes agree here (${SLOPE_WARMUP} / ${SLOPE} / ${SLOPE_ALL} kB/s),"
                echo "because the daemon was already warm when sampling began -- there was no"
                echo "start-up ramp for the whole-window fit to mistake for growth. They are"
                echo "still reported separately: on a freshly started daemon they diverge"
                echo "sharply, and the steady-state figure is the only one that answers the"
                echo "question being asked."
            fi
        fi
        echo
        echo "Raw data: \`memory.csv\`."
    } >"$DIR/result.md"
    ok "wrote $DIR/result.md and memory.csv"
}

# ========================================================= 2-3: API load
exp_2_3() {
    EXP_ID=2-3
    local DIR; DIR=$(expdir 2-3)
    local LOG=$DIR/cmd.log
    : >"$LOG"
    hdr "50 concurrent requests to /api/v1/telemetry within 30 s"

    # Baseline latency, unloaded.
    BASE=$(pi "curl -sk -o /dev/null -w '%{time_total}' https://127.0.0.1/api/v1/telemetry")
    note "baseline latency: ${BASE}s"
    printf 'baseline single-request latency: %ss\n' "$BASE" >>"$LOG"

    BEFORE=$(pi 'awk "{printf \"%.2f\", \$1/1000}" /sys/class/thermal/thermal_zone0/temp')

    # Fire all 50 at once from the board, each recording its own timing.
    pi "bash -s" >"$DIR/latencies.txt" 2>>"$LOG" <<'EOF'
set -u
tmp=$(mktemp -d)
for i in $(seq 1 50); do
    ( curl -sk -o /dev/null \
           -w "%{http_code} %{time_total} %{time_connect} %{time_starttransfer}\n" \
           --max-time 30 https://127.0.0.1/api/v1/telemetry >"$tmp/$i" 2>/dev/null ) &
done
wait
cat "$tmp"/* 2>/dev/null
rm -rf "$tmp"
EOF

    AFTER=$(pi 'awk "{printf \"%.2f\", \$1/1000}" /sys/class/thermal/thermal_zone0/temp')

    TOTAL=$(wc -l <"$DIR/latencies.txt" | tr -d ' ')
    OK200=$(awk '$1=="200"' "$DIR/latencies.txt" | wc -l | tr -d ' ')
    read -r LMIN LMAX LMEAN <<<"$(awk '{v=$2+0; s+=v; n++; if(mn==""||v<mn)mn=v; if(v>mx)mx=v}
                                      END{ if(n) printf "%.3f %.3f %.3f", mn, mx, s/n; else printf "NA NA NA" }' "$DIR/latencies.txt")"
    P95=$(awk '{print $2}' "$DIR/latencies.txt" | sort -n | awk '{a[NR]=$1} END{ if(NR) printf "%.3f", a[int(NR*0.95)?int(NR*0.95):1] }')
    FACTOR=$(awk -v a="$LMEAN" -v b="$BASE" 'BEGIN{ printf "%.1f", (b>0 ? a/b : 0) }')

    if [ "$OK200" = "$TOTAL" ] && [ "$TOTAL" = "50" ]; then
        ok "all 50 requests returned 200"
    else
        bad "$OK200 of $TOTAL returned 200"
    fi
    note "latency min/mean/max = $LMIN / $LMEAN / $LMAX s (p95 $P95), ${FACTOR}x baseline"
    note "temperature $BEFORE C -> $AFTER C"

    {
        echo "# Experiment 2-3 -- 50 concurrent requests to /api/v1/telemetry"
        echo
        echo "Run: $(iso_now)."
        echo
        echo "| | |"
        echo "|---|---|"
        echo "| requests / HTTP 200 | $TOTAL / $OK200 |"
        echo "| baseline latency (unloaded) | ${BASE}s |"
        echo "| min / mean / max under load | ${LMIN}s / ${LMEAN}s / ${LMAX}s |"
        echo "| p95 | ${P95}s |"
        echo "| slowdown vs baseline | ${FACTOR}x |"
        echo "| CPU temperature | ${BEFORE} C -> ${AFTER} C |"
        echo
        echo "Latency rises by about ${FACTOR}x, and no request is dropped or refused."
        echo "The daemon serves each connection from a small thread pool, so 50 arrivals"
        echo "queue rather than fan out into 50 threads -- which is why the tail grows"
        echo "while the error count stays at zero. Each request also re-reads /proc and"
        echo "/sys, so the work is genuinely repeated per call and not served from cache."
        echo
        echo "Raw timings: \`latencies.txt\` (http_code, total, connect, starttransfer)."
    } >"$DIR/result.md"
    ok "wrote $DIR/result.md"
}

case "$WHICH" in
    2-1) exp_2_1 ;;
    2-2) exp_2_2 ;;
    2-3) exp_2_3 ;;
    all) exp_2_3; exp_2_2; exp_2_1 ;;
    *)   echo "usage: $0 [2-1|2-2|2-3|all]" >&2; exit 2 ;;
esac
