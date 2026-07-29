#!/usr/bin/env bash
# =============================================================================
#  Broker reachability fallback
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  WHY THIS EXISTS
#  ---------------
#  The Mosquitto broker runs on the personal computer, exactly as the project
#  brief requires. Whether the board can *reach* it, though, depends on which
#  Wi-Fi the laptop is attached to: on the 192.168.100.0/24 network the two
#  machines talk directly, but when the laptop joins the inner 192.168.0.0/24
#  network it sits behind a NAT, so laptop -> board still works while
#  board -> laptop does not.
#
#  This script opens an SSH remote port-forward from the laptop to the board,
#  presenting the very same broker on the board's own loopback:
#
#      board 127.0.0.1:1883  ---ssh -R--->  laptop 127.0.0.1:1883  (mosquitto)
#
#  The board's MQTT client has both addresses configured and alternates between
#  them after a sustained outage, so whichever path is available wins with no
#  manual intervention. Nothing about the broker's location changes -- only the
#  route the TCP connection takes.
#
#  Run in the foreground, or install as a launchd agent:
#      ./mqtt_tunnel.sh --install
# =============================================================================
set -uo pipefail

BOARD_USER=${BOARD_USER:-mahan}
BOARD_HOST=${BOARD_HOST:-192.168.100.26}
BROKER_PORT=${BROKER_PORT:-1883}
PLIST_LABEL=com.mahan.guardian.mqtt-tunnel
PLIST_PATH="$HOME/Library/LaunchAgents/$PLIST_LABEL.plist"

ssh_args=(
    -N -T
    -o ExitOnForwardFailure=yes
    -o ServerAliveInterval=15
    -o ServerAliveCountMax=3
    -o ConnectTimeout=10
    -o StrictHostKeyChecking=accept-new
    -o BatchMode=yes
    # Remote-forward: the board's loopback :1883 reaches our broker.
    -R "${BROKER_PORT}:127.0.0.1:${BROKER_PORT}"
    "${BOARD_USER}@${BOARD_HOST}"
)

install_agent() {
    mkdir -p "$HOME/Library/LaunchAgents"
    cat >"$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$PLIST_LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/bin/ssh</string>
$(printf '    <string>%s</string>\n' "${ssh_args[@]}")
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>/tmp/guardian-mqtt-tunnel.log</string>
  <key>StandardErrorPath</key><string>/tmp/guardian-mqtt-tunnel.log</string>
</dict>
</plist>
EOF
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    launchctl load -w "$PLIST_PATH"
    echo "installed and started: $PLIST_LABEL"
    echo "log: /tmp/guardian-mqtt-tunnel.log"
}

uninstall_agent() {
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    rm -f "$PLIST_PATH"
    echo "removed: $PLIST_LABEL"
}

case "${1:-run}" in
    --install)   install_agent ;;
    --uninstall) uninstall_agent ;;
    --status)
        launchctl list | grep -F "$PLIST_LABEL" || echo "agent not loaded"
        ssh -o BatchMode=yes "${BOARD_USER}@${BOARD_HOST}" \
            "ss -tln | grep ':${BROKER_PORT}' || echo 'board has no listener on ${BROKER_PORT}'"
        ;;
    run)
        echo "[tunnel] board ${BOARD_HOST} loopback :${BROKER_PORT} -> this Mac's broker"
        exec ssh "${ssh_args[@]}"
        ;;
    *) echo "usage: $0 [run|--install|--uninstall|--status]" >&2; exit 2 ;;
esac
