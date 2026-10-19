#!/bin/bash
# Render the hicolor PNGs in this folder from packaging/rune.svg. Run it
# after changing the SVG, and commit the result.
#
#   packaging/icons/render.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
for size in 16 24 32 48 64 128 256 512; do
    mkdir -p "hicolor/${size}x${size}/apps"
    rsvg-convert -w "$size" -h "$size" ../rune.svg -o "hicolor/${size}x${size}/apps/rune.png"
done
