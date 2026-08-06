#!/usr/bin/env bash
# =============================================================================
#  Exercises the OUTBOUND Telegram channel and reports on the inbound one.
#
#  Outbound is testable from here: the `snapshot` command pushes the current
#  annotated frame down both alert channels, so this and test_email.sh examine
#  the same API call from two ends.
#
#  Inbound (/preview) is not. It needs a real message from a real Telegram
#  client, which no amount of shell can fake -- so this script reports whether
#  the poller is listening and what it has handled, and then tells you what to
#  send from your phone rather than pretending to have tested it.
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

echo "asking the board to send the current frame to Telegram ..."
curl -sk -X POST "https://${BOARD_HOST}/api/v1/command" \
     -H "Authorization: Bearer $TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"cmd":"snapshot"}' | python3 -m json.tool

# The channel is off unless the daemon found a bot token at startup; the
# command still succeeds in that case, so say so rather than letting the user
# wait for a photo that was never going to arrive.
#
# A daemon predating the Telegram channel has no such component at all, and
# that is the single most likely reason this script comes up empty -- so it is
# reported as its own outcome rather than as a missing key.
echo
echo "channel state:"
ssh -o BatchMode=yes "$BOARD" "curl -sk https://127.0.0.1/api/v1/health" |
    python3 -c '
import json, sys
try:
    h = json.load(sys.stdin)
except ValueError:
    print("   could not parse /api/v1/health"); sys.exit(0)
t = h.get("components", {}).get("telegram")
if t is None:
    print("   NOT DEPLOYED -- this daemon has no Telegram channel.")
    print("   The running binary predates it; rebuild and restart the board.")
    sys.exit(0)
print("   send : ok=%s state=%s failures=%s"
      % (t.get("ok"), t.get("state"), t.get("consecutive_failures")))
c = t.get("commands")
if c is None:
    print("   recv : NOT DEPLOYED -- this build has no command poller.")
elif not c.get("polling"):
    print("   recv : NOT LISTENING -- telegram_commands_enabled is false,")
    print("          or the poller thread failed to start. Check the journal.")
else:
    print("   recv : listening, handled=%s poll_failures=%s last=%s"
          % (c.get("handled"), c.get("poll_failures"), c.get("last_command")))' || true

# Longer than the mail test: a send through the SOCKS proxy can take a few
# seconds, and a first attempt that fails is retried once after three.
echo
echo "waiting for the telegram worker ..."
sleep 20
ssh -o BatchMode=yes "$BOARD" \
    'journalctl -u guardian -n 80 --no-pager -o cat | grep -i "telegram" | tail -8'

cat <<'EOF'

Reading the result
------------------
  telegram: "..." delivered ... (with photo)
        the photo is in the chat.

  telegram: '...' suppressed (same kind sent 40s ago)
        the 120 s per-kind floor, not a fault. Running test_email.sh and this
        script back to back trips it, because both send the same `snapshot`
        command. Wait two minutes and run it again.

  telegram: channel disabled, not starting a worker
        GUARDIAN_TELEGRAM_TOKEN is unset, or telegram_chat_id is empty in
        /etc/guardian/guardian.conf.  ./code/mac/set_secret.sh GUARDIAN_TELEGRAM_TOKEN

  telegram: transport failed: ...
        the board could not reach the proxy at all. Check telegram_proxy_url
        and GUARDIAN_TELEGRAM_PROXY_PASS, then test the proxy on its own:
            ssh -t mahan@mahan.local
            sudo -n bash -c 'set -a; . /etc/guardian/secrets.env;
              curl -s --socks5-hostname \
                "1:$GUARDIAN_TELEGRAM_PROXY_PASS@proxy.example.invalid:1080" \
                "https://api.telegram.org/bot$GUARDIAN_TELEGRAM_TOKEN/getMe"'

  telegram: API rejected the message (HTTP 401): Unauthorized
        the bot token is wrong or has been revoked in @BotFather.

  telegram: API rejected the message (HTTP 400): chat not found
        telegram_chat_id is wrong, or you have never sent the bot a message --
        a bot cannot open a conversation, so send it /start once first.

Inbound commands -- test these from your phone
----------------------------------------------
  /preview   a photo of the current frame. Send it twice quickly: the second
             should come back as "Slow down -- try again in N s" rather than a
             second photo, and must not raise telegram.sent.
  /help      the command list.
  /start     the greeting.
  /nonsense  should answer "I do not know /nonsense" and raise
             commands.rejected in /api/v1/stats.

  Then watch what the board made of it:
      ssh mahan@mahan.local 'journalctl -u guardian -f -o cat | grep -i telegram'

  telegram: getUpdates conflict (HTTP 409)
        two daemons are polling the same bot, or a webhook is set. Only one
        consumer per bot is allowed.
EOF
