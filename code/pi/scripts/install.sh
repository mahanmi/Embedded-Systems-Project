#!/usr/bin/env bash
# =============================================================================
#  Builds and installs the guardian stack on the board.
#
#  Run from the source directory:   sudo bash scripts/install.sh
#  Assumes scripts/bootstrap.sh has already prepared the machine.
# =============================================================================
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PREFIX=/opt/guardian
CONFDIR=/etc/guardian

log() { printf '\033[1;36m[install]\033[0m %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }

# ------------------------------------------------------------------- build
log "building the C daemon"
make -C "$SRC" -j"$(nproc)" >/dev/null
log "built $(stat -c%s "$SRC/guardian") bytes"

# ----------------------------------------------------------------- binaries
log "installing binaries and assets"
install -D -m 0755 "$SRC/guardian"                    "$PREFIX/bin/guardian"
install -D -m 0755 "$SRC/vision/guardian_vision.py"   "$PREFIX/bin/guardian_vision.py"
install -D -m 0644 "$SRC/vision/shmframe.py"          "$PREFIX/bin/shmframe.py"
install -D -m 0755 "$SRC/api/guardian_api.py"         "$PREFIX/bin/guardian_api.py"
install -D -m 0644 "$SRC/www/index.html"              "$PREFIX/www/index.html"
chown -R guardian:guardian "$PREFIX"

# ---------------------------------------------------------------- config
# Never clobber a live configuration; drop the new one alongside instead.
if [ -f "$CONFDIR/guardian.conf" ]; then
    install -D -m 0640 -o root -g guardian "$SRC/config/guardian.conf" \
            "$CONFDIR/guardian.conf.new"
    if cmp -s "$CONFDIR/guardian.conf" "$CONFDIR/guardian.conf.new"; then
        rm -f "$CONFDIR/guardian.conf.new"
    else
        log "NOTE: $CONFDIR/guardian.conf.new differs from the live config"
    fi
else
    install -D -m 0640 -o root -g guardian "$SRC/config/guardian.conf" \
            "$CONFDIR/guardian.conf"
fi

# ---------------------------------------------------------------- secrets
if [ ! -f "$CONFDIR/secrets.env" ]; then
    install -D -m 0640 -o root -g guardian "$SRC/config/secrets.env.example" \
            "$CONFDIR/secrets.env"
    # Give the API token a real random value up front; the two passwords must
    # be filled in by hand.
    TOKEN=$(openssl rand -hex 24)
    sed -i "s|^GUARDIAN_API_TOKEN=.*|GUARDIAN_API_TOKEN=$TOKEN|" \
        "$CONFDIR/secrets.env"
    log "created $CONFDIR/secrets.env with a generated API token"
    log "   -> you must still set GUARDIAN_SMTP_PASS and GUARDIAN_MQTT_PASS"
fi

# ------------------------------------------------------------------- certs
if [ ! -f "$CONFDIR/certs/guardian.crt" ]; then
    log "generating the self-signed certificate"
    bash "$SRC/scripts/gen_cert.sh"
fi

# ------------------------------------------------------------------ models
if [ ! -f "$PREFIX/models/MobileNetSSD_deploy.caffemodel" ]; then
    log "fetching the detection model"
    bash "$SRC/scripts/fetch_model.sh" "$PREFIX/models"
fi

# ------------------------------------------------------------------ polkit
log "installing the polkit rule"
install -D -m 0644 "$SRC/config/49-guardian.rules" \
        /etc/polkit-1/rules.d/49-guardian.rules

# ----------------------------------------------------------------- systemd
log "installing systemd units"
for u in guardian-ingest.socket guardian-vision.service guardian.service \
         guardian-api.service; do
    install -D -m 0644 "$SRC/systemd/$u" "/etc/systemd/system/$u"
done

systemctl daemon-reload
systemctl enable guardian-ingest.socket guardian-vision.service \
                 guardian.service guardian-api.service >/dev/null

log "installed. Start with:"
log "    sudo systemctl restart guardian-ingest.socket guardian-vision guardian guardian-api"
