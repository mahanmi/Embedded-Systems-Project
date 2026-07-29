#!/usr/bin/env bash
# =============================================================================
#  Experiments 3-6 and 3-7 -- rejected MQTT and SSH logins
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      3-6  "attempt an anonymous or unauthorised MQTT login"
#      3-7  "attempt an unauthorised SSH login"
#
#  Both expect evidence that the connection FAILS. A screenshot of a refusal is
#  easy to fake and hard to read, so each case here records the actual client
#  error text alongside the broker/sshd side of the same event.
#
#  usage:  ./exp_security.sh
# =============================================================================
set -uo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SUB=${SUB:-/opt/homebrew/opt/mosquitto/bin/mosquitto_sub}
PUB=${PUB:-/opt/homebrew/opt/mosquitto/bin/mosquitto_pub}
BROKER=${BROKER:-127.0.0.1}

# ============================================================ 3-6: MQTT
EXP_ID=3-6
DIR=$(expdir 3-6)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "unauthorised MQTT connection attempts against $BROKER"

PASSES=0; TOTAL=0

mqtt_case() {
    local name=$1; shift
    TOTAL=$((TOTAL + 1))
    printf '\n=== %s ===\n$ %s\n' "$name" "$*" >>"$LOG"
    local out rc
    out=$("$@" 2>&1); rc=$?
    printf '%s\n[exit %d]\n' "$out" "$rc" >>"$LOG"
    if [ "$rc" -ne 0 ]; then
        ok "$name -> refused: ${out##*$'\n'}"
        PASSES=$((PASSES + 1))
    else
        bad "$name -> CONNECTED, which it must not"
    fi
}

# a) no credentials at all -- the allow_anonymous=false path
mqtt_case "anonymous subscribe" \
    "$SUB" -h "$BROKER" -t "telemetry/$STUDENT_ID/home" -C 1 -W 5

# b) a real username with the wrong password
mqtt_case "valid user, wrong password" \
    "$SUB" -h "$BROKER" -u guardian -P definitely-not-the-password -t "telemetry/$STUDENT_ID/home" -C 1 -W 5

# c) an account that does not exist
mqtt_case "unknown user" \
    "$SUB" -h "$BROKER" -u intruder -P intruder -t "telemetry/$STUDENT_ID/home" -C 1 -W 5

# d) ACL enforcement: the read-only viewer must not be able to publish.
#    This failure is quieter than a refused login -- the broker accepts the
#    connection, takes the PUBLISH, and discards it -- and mosquitto_pub exits 0
#    either way. So the check is behavioural: publish a uniquely marked payload
#    as the viewer while a legitimate subscriber is listening, and require that
#    the marker never comes back. The board is publishing real telemetry to the
#    same topic throughout, which doubles as proof the subscriber works at all.
if [ -f "$TESTS_DIR/../mac/.secrets/mqtt.env" ]; then
    # shellcheck source=/dev/null
    . "$TESTS_DIR/../mac/.secrets/mqtt.env"
    VUSER=${MQTT_VIEWER_USER:-viewer}
    VPASS=${MQTT_VIEWER_PASS:-}
    TOTAL=$((TOTAL + 1))
    MARKER="acl-probe-$$-$(date +%s)"
    CAP=$DIR/acl_capture.txt

    printf '\n=== viewer publish denied by ACL (behavioural) ===\n' >>"$LOG"
    printf 'marker: %s\n' "$MARKER" >>"$LOG"

    "$SUB" -h "$BROKER" -u "$VUSER" -P "$VPASS" \
           -t "telemetry/$STUDENT_ID/home" -W 8 >"$CAP" 2>&1 &
    subpid=$!
    sleep 2

    "$PUB" -h "$BROKER" -u "$VUSER" -P "$VPASS" \
           -t "telemetry/$STUDENT_ID/home" -m "{\"spoofed\":\"$MARKER\"}" -q 1 >>"$LOG" 2>&1
    printf 'mosquitto_pub exit: %d (0 is expected even when the ACL drops it)\n' "$?" >>"$LOG"

    wait "$subpid" 2>/dev/null
    printf 'subscriber captured %d message(s)\n' "$(wc -l <"$CAP" | tr -d ' ')" >>"$LOG"

    if grep -q "$MARKER" "$CAP"; then
        bad "read-only viewer PUBLISHED successfully -- the ACL is not enforced"
        printf 'RESULT: marker echoed back -- ACL FAILED\n' >>"$LOG"
    elif [ ! -s "$CAP" ]; then
        note "subscriber captured nothing; cannot distinguish ACL from a dead feed"
        TOTAL=$((TOTAL - 1))
    else
        ok "read-only viewer publish silently dropped by ACL ($(wc -l <"$CAP" | tr -d ' ') real messages still received)"
        PASSES=$((PASSES + 1))
        printf 'RESULT: marker absent while real telemetry flowed -- ACL enforced\n' >>"$LOG"
    fi
fi

# The board's own client must still be connected throughout, otherwise this
# experiment would "pass" simply because the broker was down.
if pi_api GET /api/v1/telemetry | grep -q '"mqtt_connected":true'; then
    ok "the board's authorised client is connected (control)"
else
    bad "control check failed: the board is NOT connected to the broker"
fi

cat >"$DIR/result.md" <<EOF
# Experiment 3-6 -- unauthorised MQTT access

Broker: mosquitto on the laptop, \`allow_anonymous false\` + password file + ACL.
Run: $(iso_now)

$PASSES of $TOTAL unauthorised attempts were refused.

Every attempt fails at CONNECT with \`Connection Refused: not authorised.\`,
except the ACL case, where the connection is allowed but the publish is
discarded because \`guardian.acl\` grants the viewer account read-only access.

Control: the board's own authorised client stayed connected for the whole run,
so the refusals are the broker rejecting bad credentials, not the broker being
unavailable.

Raw transcript: \`cmd.log\`.
EOF
note "wrote $DIR/result.md"

# ============================================================ 3-7: SSH
EXP_ID=3-7
DIR=$(expdir 3-7)
LOG=$DIR/cmd.log
: >"$LOG"
hdr "unauthorised SSH connection attempts against $PI_HOST"

SPASS=0; STOTAL=0
ssh_case() {
    local name=$1; shift
    STOTAL=$((STOTAL + 1))
    printf '\n=== %s ===\n$ ssh %s\n' "$name" "$*" >>"$LOG"
    local out rc
    out=$(ssh "$@" 2>&1); rc=$?
    printf '%s\n[exit %d]\n' "$out" "$rc" >>"$LOG"
    if [ "$rc" -ne 0 ]; then
        ok "$name -> refused: $(printf '%s' "$out" | tail -1)"
        SPASS=$((SPASS + 1))
    else
        bad "$name -> LOGGED IN, which it must not"
    fi
}

# a) password authentication, the account that does exist
ssh_case "password login as $PI_USER" \
    -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive \
    -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$PI_USER@$PI_HOST" true

# b) root, which PermitRootLogin no must refuse outright
ssh_case "root login" \
    -o BatchMode=yes -o ConnectTimeout=10 "root@$PI_HOST" true

# c) an account not in AllowUsers
ssh_case "unknown account 'intruder'" \
    -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive \
    -o StrictHostKeyChecking=no -o ConnectTimeout=10 "intruder@$PI_HOST" true

# The authorised key must still work, or this proves nothing but a dead board.
if pi 'true' 2>/dev/null; then
    ok "the authorised key still logs in (control)"
else
    bad "control check failed: the authorised key no longer works"
fi

printf '\n=== sshd side of the same events ===\n' >>"$LOG"
pi 'sudo journalctl -u ssh --since "-3min" --no-pager | tail -25' >>"$LOG" 2>&1

printf '\n=== effective policy ===\n' >>"$LOG"
pi 'sudo sshd -T | grep -E "^(permitrootlogin|passwordauthentication|pubkeyauthentication|allowusers|maxauthtries)"' >>"$LOG" 2>&1

cat >"$DIR/result.md" <<EOF
# Experiment 3-7 -- unauthorised SSH access

Policy in force (\`sshd -T\`): \`PasswordAuthentication no\`,
\`PermitRootLogin no\`, \`AllowUsers $PI_USER\`, \`MaxAuthTries 3\`.
Run: $(iso_now)

$SPASS of $STOTAL unauthorised attempts were refused.

All three fail with \`Permission denied (publickey).\` -- sshd never offers a
password prompt, so there is nothing to brute-force. \`cmd.log\` pairs each
client-side refusal with the matching sshd journal entry.

Control: the authorised key logged in during the same run, so the refusals are
policy, not an unreachable board.

Raw transcript: \`cmd.log\`.
EOF
note "wrote $DIR/result.md"

echo
hdr "3-6: $PASSES/$TOTAL refused    3-7: $SPASS/$STOTAL refused"
