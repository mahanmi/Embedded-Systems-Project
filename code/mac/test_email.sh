#!/usr/bin/env bash
# =============================================================================
#  Triggers one email from the board's C mailer and reports the outcome.
#  Uses the `snapshot` command, which mails the current annotated frame.
# =============================================================================
set -uo pipefail

# Bonjour name rather than a literal IP -- the board is a DHCP client and its
# address moves (.26 -> .33 on 2026-08-02). See the note in stream_dvr.sh.
BOARD_HOST=${BOARD_HOST:-mahan.local}
BOARD_USER=${BOARD_USER:-mahan}
BOARD="${BOARD_USER}@${BOARD_HOST}"

TOKEN=$(ssh -o BatchMode=yes "$BOARD" \
        'sudo -n grep GUARDIAN_API_TOKEN /etc/guardian/secrets.env 2>/dev/null | cut -d= -f2') || true
if [ -z "${TOKEN:-}" ]; then
    echo "could not read the API token from the board (sudo may need priming:" >&2
    echo "    ssh -t $BOARD 'sudo -v'" >&2
    exit 1
fi

echo "asking the board to email the current frame ..."
curl -sk -X POST "https://${BOARD_HOST}/api/v1/command" \
     -H "Authorization: Bearer $TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"cmd":"snapshot"}' | python3 -m json.tool

echo
echo "waiting for the mail worker ..."
sleep 12
ssh -o BatchMode=yes "$BOARD" \
    'journalctl -u guardian -n 60 --no-pager -o cat | grep -i "mailer:" | tail -5'

echo
echo 'A line reading  mailer: "..." delivered  means Gmail accepted the message.'
echo 'A line reading  send failed: Login denied  means GUARDIAN_SMTP_PASS is wrong;'
echo 'set it with:  ./code/mac/set_secret.sh GUARDIAN_SMTP_PASS'
