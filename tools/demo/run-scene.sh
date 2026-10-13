#!/bin/bash
# Plays one scene against the real app, offscreen, with the demo theme and
# settings in tools/demo/work/home (your own are untouched). Use make-media.py
# for the whole set.
#
#   tools/demo/run-scene.sh scenes/build-novel.json [scale]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$here/work"
scene="$(realpath "$1")"
scale="${2:-1}"
mkdir -p "$work/home/.local/state/omarchy/current/theme" "$work/home/.config/Rune" "$work/home/.local/share" "$work/out"
cp "$here/theme/colors.toml" "$work/home/.local/state/omarchy/current/theme/"
[ -L "$work/home/.local/share/fonts" ] || ln -s "$HOME/.local/share/fonts" "$work/home/.local/share/fonts"
[ -L "$work/home/.config/fontconfig" ] || ln -s "$HOME/.config/fontconfig" "$work/home/.config/fontconfig"
[ -f "$work/home/.config/Rune/Rune.conf" ] || printf '[editor]\nautosave=false\n' > "$work/home/.config/Rune/Rune.conf"
HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" \
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR="$scale" QT_PLUGIN_PATH="$here/build/plugins" QT_FORCE_STDERR_LOGGING=1 \
RUNE_DEMO_SCRIPT="$scene" RUNE_DEMO_OUT="$work/out" \
    "${RUNE_BIN:-$here/../../build/native/rune}" -plugin runedemo
