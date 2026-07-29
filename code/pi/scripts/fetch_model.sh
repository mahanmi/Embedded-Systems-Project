#!/usr/bin/env bash
# =============================================================================
#  Downloads the MobileNet-SSD person-detection model into /opt/guardian/models
#
#  MobileNet-SSD (Caffe, PASCAL VOC, 21 classes) was chosen over YOLO/SSD-v2
#  because it is the only widely-available detector that runs at a usable rate
#  on a Raspberry Pi 3B+ (Cortex-A53, no NEON-optimised NPU): ~23 MB of weights
#  and a 300x300 input. Class 15 is `person`.
# =============================================================================
set -euo pipefail

MODELDIR=${1:-/opt/guardian/models}
BASE=https://github.com/PINTO0309/MobileNet-SSD-RealSense/raw/master/caffemodel/MobileNetSSD

# Expected sizes, used as a cheap integrity check against truncated downloads
# and HTML error pages served with a 200.
declare -A EXPECT=(
    [MobileNetSSD_deploy.prototxt]=29366
    [MobileNetSSD_deploy.caffemodel]=23147564
)

install -d "$MODELDIR"

for f in "${!EXPECT[@]}"; do
    dst="$MODELDIR/$f"
    want=${EXPECT[$f]}
    if [ -f "$dst" ] && [ "$(stat -c%s "$dst")" -eq "$want" ]; then
        echo "[model] $f already present ($want bytes)"
        continue
    fi
    echo "[model] downloading $f ..."
    curl -fsSL --retry 3 --retry-delay 2 -o "$dst.tmp" "$BASE/$f"
    got=$(stat -c%s "$dst.tmp")
    if [ "$got" -ne "$want" ]; then
        rm -f "$dst.tmp"
        echo "[model] ERROR: $f is $got bytes, expected $want" >&2
        exit 1
    fi
    mv "$dst.tmp" "$dst"
    echo "[model] $f OK ($got bytes)"
done

chown -R guardian:guardian "$MODELDIR" 2>/dev/null || true
ls -l "$MODELDIR"
