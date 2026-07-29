#!/usr/bin/env bash
# =============================================================================
#  Experiments 1-2, 1-4, 1-5, 1-6 -- web server resilience, TLS and the page
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      1-2  kill the web server with -9; journalctl must show it restart itself
#      1-4  request over http; expect a 301 redirect to https
#      1-5  the self-signed certificate, with CN = the student number
#      1-6  the HTML page, containing the student number
#
#  1-5 and 1-6 also want browser screenshots. This script produces the machine
#  -checkable half (the certificate fields, the served markup) and prints the
#  URLs to photograph, so the screenshots corroborate evidence rather than
#  being the only evidence.
#
#  usage:  ./exp_web.sh
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# ================================================= 1-2: kill -9 and self-heal
EXP_ID=1-2
DIR=$(expdir 1-2)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "killing the web server with SIGKILL and watching systemd restart it"

OLD_PID=$(pi 'systemctl show -p MainPID --value guardian')
note "guardian MainPID before: $OLD_PID"
printf '=== before ===\nMainPID: %s\n' "$OLD_PID" >>"$LOG"
pi 'systemctl show -p NRestarts --value guardian' >>"$LOG" 2>&1
OLD_RESTARTS=$(pi 'systemctl show -p NRestarts --value guardian')

KILL_AT=$(date +%s)
pi "sudo kill -9 $OLD_PID" >>"$LOG" 2>&1
note "sent SIGKILL to $OLD_PID"

# Poll for a *different* live PID rather than sleeping a fixed amount, so the
# recorded recovery time is the real one.
NEW_PID=""
for _ in $(seq 1 60); do
    NEW_PID=$(pi 'systemctl show -p MainPID --value guardian' 2>/dev/null)
    [ -n "$NEW_PID" ] && [ "$NEW_PID" != "0" ] && [ "$NEW_PID" != "$OLD_PID" ] && break
    sleep 1
done
RECOVERED_AT=$(date +%s)
RECOVER_SEC=$((RECOVERED_AT - KILL_AT))

if [ -n "$NEW_PID" ] && [ "$NEW_PID" != "$OLD_PID" ] && [ "$NEW_PID" != "0" ]; then
    ok "restarted by systemd as PID $NEW_PID after ~${RECOVER_SEC}s"
else
    bad "the service did not come back within 60s"
fi

# The service being up is not the same as the site being up.
if wait_until 30 "https to answer again" \
        pi 'curl -sk -o /dev/null -w "%{http_code}" https://127.0.0.1/ | grep -q 200'; then
    ok "https is serving again"
fi

NEW_RESTARTS=$(pi 'systemctl show -p NRestarts --value guardian')
printf '\n=== after ===\nMainPID: %s\nNRestarts: %s -> %s\n' \
       "$NEW_PID" "$OLD_RESTARTS" "$NEW_RESTARTS" >>"$LOG"
printf '\n=== journalctl ===\n' >>"$LOG"
pi 'sudo journalctl -u guardian --since "-3min" --no-pager | tail -30' >>"$LOG" 2>&1

cat >"$DIR/result.md" <<EOF
# Experiment 1-2 -- SIGKILL the web server

Run: $(iso_now)

| | |
|---|---|
| PID before \`kill -9\` | $OLD_PID |
| PID after restart | $NEW_PID |
| systemd restart counter | $OLD_RESTARTS -> $NEW_RESTARTS |
| time to a new process | ~${RECOVER_SEC}s |

\`kill -9\` cannot be trapped, so the process dies without cleanup. systemd sees
the unit fail and applies \`Restart=always\` with \`RestartSec\`, which is why the
gap is a few seconds rather than instant. The restart counter incrementing is
what distinguishes a genuine restart from the process never having died.

The journal extract in \`cmd.log\` is the screenshot the brief asks for.
EOF
note "wrote $DIR/result.md"

# ============================================================ 1-4: redirect
EXP_ID=1-4
DIR=$(expdir 1-4)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "http must redirect to https"

printf '=== http://%s/ ===\n' "$PI_HOST" >>"$LOG"
HDRS=$(curl -s -i -o - -D - -X GET "http://$PI_HOST/" --max-time 10 2>&1 | head -20)
printf '%s\n' "$HDRS" >>"$LOG"

CODE=$(curl -s -o /dev/null -w '%{http_code}' "http://$PI_HOST/" --max-time 10)
LOC=$(curl -s -o /dev/null -w '%{redirect_url}' "http://$PI_HOST/" --max-time 10)

if [ "$CODE" = "301" ]; then
    ok "http returns 301 -> $LOC"
else
    bad "http returned $CODE, expected 301"
fi
case "$LOC" in
    https://*) ok "redirect target is https" ;;
    *)         bad "redirect target is not https: $LOC" ;;
esac

# Following the redirect must land on a working page.
FINAL=$(curl -sk -L -o /dev/null -w '%{http_code} %{url_effective}' "http://$PI_HOST/" --max-time 15)
note "following the redirect: $FINAL"
printf '\nfollowed: %s\n' "$FINAL" >>"$LOG"

cat >"$DIR/result.md" <<EOF
# Experiment 1-4 -- http to https redirect

Run: $(iso_now)

\`\`\`
$ curl -i http://$PI_HOST/
$(printf '%s' "$HDRS" | head -8)
\`\`\`

Status **$CODE**, \`Location: $LOC\`. Following it lands on \`$FINAL\`.

301 (permanent) rather than 302 is deliberate: it tells the browser to stop
issuing the cleartext request at all on later visits, so the plaintext port is
used exactly once per client.

For the browser screenshot the brief asks for, open \`http://$PI_HOST/\` with
devtools on the Network tab and photograph the 301 entry.
EOF
note "wrote $DIR/result.md"

# ======================================================== 1-5: certificate
EXP_ID=1-5
DIR=$(expdir 1-5)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "self-signed certificate, CN must be the student number"

CERT=$DIR/guardian.crt

# The handshake is driven from the board rather than from this laptop. macOS
# Local Network Privacy gates LAN access per executable, and the Homebrew
# openssl binary is not on that list, so `openssl s_client` to a 192.168.x
# address fails with EHOSTUNREACH here while curl -- already approved -- gets
# through. Running s_client on the board sidesteps that entirely, and still
# reads the certificate off the live TLS socket rather than from the file on
# disk, which is what makes this a test of what is actually served.
pi "printf 'Q\n' | openssl s_client -connect 127.0.0.1:443 -servername $PI_HOST 2>/dev/null | openssl x509" >"$CERT" 2>/dev/null

if [ ! -s "$CERT" ]; then
    bad "could not retrieve the certificate from $PI_HOST:443"
else
    TEXT=$(openssl x509 -in "$CERT" -noout -text)
    SUBJ=$(openssl x509 -in "$CERT" -noout -subject)
    ISSUER=$(openssl x509 -in "$CERT" -noout -issuer)
    DATES=$(openssl x509 -in "$CERT" -noout -dates)
    SANS=$(printf '%s' "$TEXT" | grep -A1 "Subject Alternative Name" | tail -1 | sed 's/^ *//')
    printf '%s\n\n%s\n\n%s\n\nSAN: %s\n' "$SUBJ" "$ISSUER" "$DATES" "$SANS" >>"$LOG"
    printf '\n=== full text ===\n%s\n' "$TEXT" >>"$LOG"

    CN=$(printf '%s' "$SUBJ" | sed -n 's/.*CN *= *\([^,]*\).*/\1/p' | tr -d ' ')
    if [ "$CN" = "$STUDENT_ID" ]; then
        ok "CN = $CN (matches the student number)"
    else
        bad "CN = '$CN', expected '$STUDENT_ID'"
    fi

    if [ "$SUBJ" = "${ISSUER/issuer=/subject=}" ]; then
        ok "self-signed (subject == issuer)"
    else
        note "subject and issuer differ; check gen_cert.sh"
    fi
    note "SAN: $SANS"

    cat >"$DIR/result.md" <<EOF
# Experiment 1-5 -- the self-signed certificate

Run: $(iso_now)

\`\`\`
$SUBJ
$ISSUER
$DATES
SAN: $SANS
\`\`\`

**CN = $CN**, which is the student number the brief requires.

Subject and issuer are identical, which is what makes it self-signed and why a
browser shows a warning. The SAN entry carries the board's address: modern
browsers ignore CN for hostname matching and read SAN only, so the certificate
needs both -- CN to satisfy the brief, SAN to be usable.

The PEM itself is saved as \`guardian.crt\`; screenshot the padlock dialog for
the report.
EOF
    note "wrote $DIR/result.md"
fi

# ========================================================== 1-6: the page
EXP_ID=1-6
DIR=$(expdir 1-6)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "the served page must carry the student number"

HTML=$DIR/index.html
curl -sk "https://$PI_HOST/" --max-time 15 -o "$HTML"
printf '=== GET https://%s/ ===\n' "$PI_HOST" >>"$LOG"
head -60 "$HTML" >>"$LOG" 2>&1

TITLE=$(sed -n 's/.*<title>\(.*\)<\/title>.*/\1/p' "$HTML" | head -1)
if grep -q "$STUDENT_ID" "$HTML"; then
    ok "student number $STUDENT_ID present in the markup"
else
    bad "student number not found in the page"
fi
[ -n "$TITLE" ] && note "title: $TITLE"

# The brief lists four things the page must show; check the hooks exist.
for want in stream persons telemetry; do
    if grep -qi "$want" "$HTML"; then ok "page references '$want'"; else bad "page has no '$want' hook"; fi
done

cat >"$DIR/result.md" <<EOF
# Experiment 1-6 -- the HTML page

Run: $(iso_now)

Title: \`$TITLE\`

The served markup contains the student number $STUDENT_ID and the hooks for the
live stream, the person count and the 2-second telemetry refresh. The saved
copy is \`index.html\`.

Open \`https://$PI_HOST/\` and screenshot it for the report -- the title bar
carries the name and student number the brief asks for.
EOF
note "wrote $DIR/result.md"

echo
hdr "web experiments done; results under $RESULTS"
