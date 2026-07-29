#!/usr/bin/env bash
# =============================================================================
#  Smart Guardian System -- board bootstrap
#  Embedded Systems final project -- Mahan Majlesi (402170516)
#
#  Prepares a fresh Ubuntu 24.04 (arm64) Raspberry Pi for the guardian stack:
#    - APT pockets + all build/runtime dependencies
#    - dedicated unprivileged system user and directory layout
#    - swap (the 1 GB Pi 3B+ ships with none)
#    - time synchronisation (needed for the MQTT one-way latency experiment)
#
#  Idempotent: safe to re-run.
# =============================================================================
set -euo pipefail

GUARDIAN_USER=guardian
PREFIX=/opt/guardian
CONFDIR=/etc/guardian
DATADIR=/var/lib/guardian
LOGDIR=/var/log/guardian
SWAPFILE=/swapfile
SWAPSIZE_MB=1024

log() { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }

[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }

# ---------------------------------------------------------------- apt pockets
# The stock Ubuntu-for-Pi image ships with only `noble` and `noble-security`.
# Some already-installed libraries come from `noble-updates`, so without that
# pocket apt sees them as newer than anything it knows about and refuses to
# resolve dependencies ("held broken packages" on e.g. bzip2 -> libbz2-1.0).
SRC=/etc/apt/sources.list.d/ubuntu.sources
if [ -f "$SRC" ] && ! grep -q 'noble-updates' "$SRC"; then
    log "adding noble-updates / noble-backports pockets"
    cp -n "$SRC" "$SRC.bak-guardian"
    sed -i 's/^Suites: noble$/Suites: noble noble-updates noble-backports/' "$SRC"
fi

log "updating package index"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq

log "installing dependencies"
apt-get install -y -qq \
    build-essential pkg-config \
    libmicrohttpd-dev libgnutls28-dev \
    libmosquitto-dev mosquitto-clients \
    libjson-c-dev libsqlite3-dev sqlite3 \
    libcurl4-openssl-dev libssl-dev \
    libsystemd-dev \
    python3-opencv python3-numpy \
    python3-fastapi python3-uvicorn python3-httpx python3-pydantic \
    v4l-utils stress-ng chrony jq bc

# ------------------------------------------------------------------ user/dirs
if ! id -u "$GUARDIAN_USER" >/dev/null 2>&1; then
    log "creating system user '$GUARDIAN_USER'"
    useradd --system --create-home --home-dir "$PREFIX" \
            --shell /usr/sbin/nologin "$GUARDIAN_USER"
fi
# Access to /dev/video* if a real camera is ever attached to the board.
usermod -aG video "$GUARDIAN_USER" || true

log "creating directory layout"
install -d -o root            -g "$GUARDIAN_USER" -m 0750 "$CONFDIR"
install -d -o "$GUARDIAN_USER" -g "$GUARDIAN_USER" -m 0755 "$PREFIX" "$PREFIX/bin" \
                                                            "$PREFIX/models" "$PREFIX/www"
install -d -o "$GUARDIAN_USER" -g "$GUARDIAN_USER" -m 0750 "$DATADIR" "$LOGDIR"

# ----------------------------------------------------------------------- swap
# 899 MB usable RAM with OpenCV + uvicorn + the C daemon is tight, and the
# stock image has no swap at all. RSS -- which the memory-leak experiment
# measures -- is unaffected by the presence of swap.
if ! swapon --show=NAME --noheadings | grep -q .; then
    log "creating ${SWAPSIZE_MB} MB swapfile"
    fallocate -l "${SWAPSIZE_MB}M" "$SWAPFILE" 2>/dev/null \
        || dd if=/dev/zero of="$SWAPFILE" bs=1M count="$SWAPSIZE_MB" status=none
    chmod 600 "$SWAPFILE"
    mkswap -q "$SWAPFILE"
    swapon "$SWAPFILE"
    grep -q "^$SWAPFILE" /etc/fstab || echo "$SWAPFILE none swap sw 0 0" >>/etc/fstab
    # Prefer reclaiming page cache over swapping the working set out.
    echo 'vm.swappiness=10' >/etc/sysctl.d/99-guardian-swap.conf
    sysctl -q -w vm.swappiness=10
fi

# ------------------------------------------------------------------------ NTP
# Experiment 3-5 measures a one-way board -> laptop MQTT delay, so the two
# clocks must be disciplined against the same source and the residual offset
# must be quantifiable.
log "enabling chrony time synchronisation"
systemctl enable --now chrony >/dev/null 2>&1 || true

log "done"
swapon --show || true
free -h
