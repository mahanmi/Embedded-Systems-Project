#!/usr/bin/env bash
# =============================================================================
#  Writes a credential into the board's /etc/guardian/secrets.env
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  The project brief forbids hard-coded passwords and API keys on the device,
#  so every credential lives only in that root-owned 0640 file, which systemd
#  injects into the daemon's environment. This helper is the safe way to put a
#  value there:
#
#    * the value is read with `read -s`, so it is never echoed;
#    * it reaches the board on the SSH channel's stdin, never as a command-line
#      argument -- arguments are visible to every user on the board through
#      `ps` and /proc/<pid>/cmdline;
#    * it never touches shell history, a temporary file on this Mac, or this
#      repository;
#    * the rewrite on the board is atomic (write to a temp file in the same
#      directory, chmod/chown, rename), so a crash cannot leave the daemon with
#      a half-written secrets file.
#
#  usage:  ./set_secret.sh GUARDIAN_SMTP_PASS
#          ./set_secret.sh GUARDIAN_MQTT_PASS
#          ./set_secret.sh GUARDIAN_API_TOKEN
# =============================================================================
set -euo pipefail

BOARD_USER=${BOARD_USER:-mahan}
BOARD_HOST=${BOARD_HOST:-192.168.100.26}
BOARD="${BOARD_USER}@${BOARD_HOST}"
REMOTE_HELPER=/tmp/guardian-set-secret.py

KEY=${1:-}
case "$KEY" in
    GUARDIAN_SMTP_PASS|GUARDIAN_MQTT_PASS|GUARDIAN_API_TOKEN) ;;
    *)
        echo "usage: $0 {GUARDIAN_SMTP_PASS|GUARDIAN_MQTT_PASS|GUARDIAN_API_TOKEN}" >&2
        exit 2 ;;
esac

if [ "$KEY" = "GUARDIAN_SMTP_PASS" ]; then
    cat <<'NOTE'
Gmail application password
--------------------------
  1. 2-step verification must be enabled on the account.
  2. Create one at  https://myaccount.google.com/apppasswords
  3. Google shows it as four groups of four letters ("abcd efgh ijkl mnop").
     Spaces are stripped below, so paste it either way.
  4. This is NOT the Google account password, and it can be revoked at any
     time from that page without affecting the account.

NOTE
fi

printf 'Value for %s (input hidden): ' "$KEY"
read -rs VALUE; echo
printf 'Confirm: '
read -rs VALUE2; echo

[ "$VALUE" = "$VALUE2" ] || { echo "values do not match" >&2; exit 1; }
[ -n "$VALUE" ] || { echo "empty value" >&2; exit 1; }
VALUE=${VALUE// /}   # Gmail displays app passwords with spaces

if [ "$KEY" = "GUARDIAN_SMTP_PASS" ] && [ ${#VALUE} -ne 16 ]; then
    echo "note: Gmail application passwords are 16 characters; got ${#VALUE}." >&2
    printf 'Continue anyway? [y/N] '
    read -r yn
    case "$yn" in [yY]) ;; *) exit 1 ;; esac
fi

# --- 1. ship the helper -----------------------------------------------------
ssh -o BatchMode=yes "$BOARD" "cat > $REMOTE_HELPER" <<'PYEOF'
import grp, os, sys

KEY = sys.argv[1]
VALUE = sys.stdin.readline().rstrip("\n")
PATH = "/etc/guardian/secrets.env"

try:
    lines = open(PATH).read().splitlines()
except FileNotFoundError:
    lines = []

out, replaced = [], False
for line in lines:
    if line.startswith(KEY + "="):
        out.append(f"{KEY}={VALUE}")
        replaced = True
    else:
        out.append(line)
if not replaced:
    out.append(f"{KEY}={VALUE}")

# Atomic replace, same filesystem, correct mode before it becomes visible.
tmp = PATH + ".new"
fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o640)
with os.fdopen(fd, "w") as fh:
    fh.write("\n".join(out) + "\n")
os.chown(tmp, 0, grp.getgrnam("guardian").gr_gid)
os.replace(tmp, PATH)

print(f"{KEY} set ({len(VALUE)} characters); {len(out)} lines in {PATH}")
PYEOF

# --- 2. prime sudo on a tty so it can prompt --------------------------------
echo "authenticating sudo on the board (your board password) ..."
ssh -t "$BOARD" 'sudo -v'

# --- 3. hand the value over on stdin ----------------------------------------
printf '%s\n' "$VALUE" |
    ssh -o BatchMode=yes "$BOARD" "sudo -n python3 $REMOTE_HELPER $KEY; rm -f $REMOTE_HELPER"

# --- 4. restart and confirm --------------------------------------------------
echo "restarting guardian.service ..."
ssh -o BatchMode=yes "$BOARD" 'sudo -n systemctl restart guardian'
sleep 5
ssh -o BatchMode=yes "$BOARD" \
    'printf "state: "; systemctl is-active guardian;
     journalctl -u guardian -n 40 --no-pager -o cat | grep -i "secrets  " | tail -1'

cat <<EOF

Done. Verify a real delivery with:

    ./code/mac/test_email.sh

EOF
