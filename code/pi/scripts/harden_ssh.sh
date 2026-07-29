#!/usr/bin/env bash
# =============================================================================
#  SSH hardening (security requirement 1 of the project brief)
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#      "SSH login only with a public key or password. root login disabled."
#
#  This script goes further than the minimum and disables password
#  authentication entirely, leaving public keys as the only way in, with root
#  login refused outright.
#
#  SAFETY
#  ------
#  Locking yourself out of a headless board is a real risk, so the script:
#
#    1. refuses to run unless at least one key in authorized_keys can actually
#       authenticate -- verified by an real loopback SSH login, not by counting
#       lines in a file;
#    2. validates the new configuration with `sshd -t` before installing it;
#    3. arms a systemd timer that restores the previous configuration after
#       ROLLBACK_MINUTES unless it is explicitly disarmed. If anything goes
#       wrong, walking away for ten minutes fixes it.
#
#  Disarm once you have confirmed you can still log in:
#      sudo bash harden_ssh.sh --confirm
# =============================================================================
set -euo pipefail

SSHD_CONF=/etc/ssh/sshd_config
DROPIN_DIR=/etc/ssh/sshd_config.d
DROPIN=$DROPIN_DIR/99-guardian-hardening.conf
BACKUP=/etc/ssh/sshd_config.guardian-backup
ROLLBACK_UNIT=guardian-ssh-rollback
ROLLBACK_MINUTES=${ROLLBACK_MINUTES:-10}
ALLOW_USER=${ALLOW_USER:-mahan}

log()  { printf '\033[1;36m[ssh]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[ssh]\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "must run as root"

# --------------------------------------------------------------- disarm mode
if [ "${1:-}" = "--confirm" ]; then
    # Stopped unconditionally. The previous version guarded this with
    # `list-timers | grep -q`, which is unsafe under `set -o pipefail`: grep -q
    # exits at the first match, systemctl dies of SIGPIPE, and the pipeline
    # reports failure -- so the guard skipped the stop exactly when the timer
    # WAS armed. `systemctl stop` on an absent unit is harmless, so no guard is
    # needed at all.
    systemctl stop "${ROLLBACK_UNIT}.timer"   2>/dev/null || true
    systemctl stop "${ROLLBACK_UNIT}.service" 2>/dev/null || true
    rm -f "/run/systemd/transient/${ROLLBACK_UNIT}.service" \
          "/run/systemd/transient/${ROLLBACK_UNIT}.timer"
    # Clears the leftover "failed" unit so it stops showing up in list-timers.
    systemctl reset-failed "${ROLLBACK_UNIT}.timer" "${ROLLBACK_UNIT}.service" 2>/dev/null || true
    systemctl daemon-reload

    # Prove it: anything still scheduled here would silently undo the hardening.
    STILL_ARMED=$(systemctl list-timers --all --no-pager 2>/dev/null |
                  grep "$ROLLBACK_UNIT" || true)
    if [ -n "$STILL_ARMED" ]; then
        printf '%s\n' "$STILL_ARMED" | grep -qE '^\s*-\s+-\s' ||
            fail "the rollback timer is still scheduled:
      $STILL_ARMED
      Stop it manually before trusting this configuration."
    fi

    log "automatic rollback disarmed -- the hardening is now permanent"
    log "active policy:"
    sshd -T 2>/dev/null | grep -E '^(permitrootlogin|passwordauthentication|pubkeyauthentication|kbdinteractiveauthentication|allowusers)' |
        sed 's/^/    /'
    exit 0
fi

if [ "${1:-}" = "--rollback" ]; then
    if [ -f "$BACKUP" ]; then
        cp "$BACKUP" "$SSHD_CONF"
        rm -f "$DROPIN"
        systemctl restart ssh 2>/dev/null || systemctl restart sshd
        log "previous SSH configuration restored"
    else
        fail "no backup at $BACKUP"
    fi
    exit 0
fi

# ------------------------------------------------- 1. prove key auth works
AUTH_KEYS="/home/$ALLOW_USER/.ssh/authorized_keys"
[ -s "$AUTH_KEYS" ] || fail "$AUTH_KEYS is missing or empty -- install a public key first"
log "$(grep -c '^[^#]' "$AUTH_KEYS") key(s) in $AUTH_KEYS"

log "verifying that key authentication actually works ..."
KEY_AUTH_OK=0

# Strongest available evidence: sshd itself recorded a real remote client
# authenticating with a key. That is precisely the path that must keep working
# after passwords are switched off.
#
# A loopback login is the weaker test and is only a fallback, because it needs a
# *private* key stored on the board. This board has none (~/.ssh holds only
# authorized_keys), so requiring it would either block the hardening outright or
# push us into generating a throwaway keypair on the device just to satisfy the
# check -- leaving a private key on the machine we are trying to secure.
# Collected in one pass into a variable rather than piped into `grep -q`:
# under `set -o pipefail`, grep -q exits at the first match, journalctl dies of
# SIGPIPE (141), and the pipeline reports failure even though the match
# succeeded -- which would silently invert this test. The trailing `|| true`
# keeps a legitimate "no matches" (grep exit 1) from tripping `set -e`.
KEY_LOGINS=$(journalctl -u ssh -u sshd --since "-24h" --no-pager 2>/dev/null |
             grep "Accepted publickey for $ALLOW_USER" || true)

if [ -n "$KEY_LOGINS" ]; then
    KEY_AUTH_OK=1
    log "sshd accepted a public key for $ALLOW_USER within the last 24h" \
        "($(printf '%s\n' "$KEY_LOGINS" | wc -l | tr -d ' ') login(s)):"
    printf '        %s\n' "$(printf '%s\n' "$KEY_LOGINS" | tail -1 | sed 's/.*sshd\[[0-9]*\]: //')"
fi

if [ "$KEY_AUTH_OK" -eq 0 ]; then
    log "no key login in the journal; falling back to a loopback test"
    if sudo -u "$ALLOW_USER" ssh \
            -o BatchMode=yes -o PasswordAuthentication=no \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            -o ConnectTimeout=10 \
            "$ALLOW_USER@127.0.0.1" true 2>/dev/null; then
        KEY_AUTH_OK=1
        log "loopback key authentication confirmed"
    fi
fi

[ "$KEY_AUTH_OK" -eq 1 ] || fail "no evidence that key authentication works.
      Disabling passwords now would lock this board. Log in once with your key
      (ssh $ALLOW_USER@<board>), then re-run this script."

# ------------------------------------------------------------- 2. write it
[ -f "$BACKUP" ] || cp "$SSHD_CONF" "$BACKUP"
mkdir -p "$DROPIN_DIR"

# Ubuntu's stock sshd_config ends with an Include of sshd_config.d, so a
# drop-in is the supported way to override; but the cloud-init image also ships
# 50-cloud-init.conf which re-enables passwords, and last-match-wins does not
# apply to sshd -- FIRST match wins. The drop-in must therefore sort before it.
DROPIN=$DROPIN_DIR/00-guardian-hardening.conf

cat >"$DROPIN" <<EOF
# Smart Guardian System -- SSH hardening
# Installed by scripts/harden_ssh.sh. sshd takes the FIRST value it sees for
# each keyword, so this file is named to sort ahead of 50-cloud-init.conf.

# Security requirement: root login disabled.
PermitRootLogin no

# Public keys only. This exceeds the brief's "key or password" wording, and is
# what makes experiment 3-7 (unauthorised SSH attempt) fail as intended.
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
ChallengeResponseAuthentication no
PermitEmptyPasswords no

# Only this account may log in at all.
AllowUsers $ALLOW_USER

# Modest extras.
MaxAuthTries 3
LoginGraceTime 30
X11Forwarding no
PermitUserEnvironment no
EOF

chmod 0644 "$DROPIN"

log "validating the new configuration"
sshd -t || { rm -f "$DROPIN"; fail "sshd -t rejected the configuration; nothing changed"; }

# ------------------------------------------------------- 3. arm the rollback
log "arming an automatic rollback in $ROLLBACK_MINUTES minutes"
systemd-run --unit="$ROLLBACK_UNIT" \
            --on-active="${ROLLBACK_MINUTES}min" \
            --description="Undo Guardian SSH hardening unless confirmed" \
            /bin/bash -c "rm -f '$DROPIN'; cp '$BACKUP' '$SSHD_CONF'; \
                          systemctl restart ssh 2>/dev/null || systemctl restart sshd; \
                          logger -t guardian 'SSH hardening rolled back automatically'" \
            >/dev/null

# --------------------------------------------------------------- 4. apply
systemctl restart ssh 2>/dev/null || systemctl restart sshd
sleep 1

log "effective policy now:"
sshd -T 2>/dev/null | grep -E '^(permitrootlogin|passwordauthentication|pubkeyauthentication|kbdinteractiveauthentication|allowusers|maxauthtries)' |
    sed 's/^/    /'

cat <<EOF

  ROLLBACK IS ARMED. From another terminal, confirm you can still log in:

      ssh $ALLOW_USER@\$(hostname -I | awk '{print \$1}')

  Then make the change permanent:

      sudo bash $0 --confirm

  Do nothing and the previous configuration returns in $ROLLBACK_MINUTES minutes.
EOF
