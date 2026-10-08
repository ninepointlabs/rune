#!/bin/bash
# Launch Rune: engine + app
ENGINE_SOCK=/tmp/rune-show.sock

# Start engine if not already running
if ! [ -S "$ENGINE_SOCK" ]; then
    /home/tim/Projects/rune/build/engine/rune-engine --socket-path "$ENGINE_SOCK" 2>/tmp/rune-engine.log &
    sleep 2
fi

# Launch app
exec /home/tim/Projects/rune/build/app/rune --socket-path "$ENGINE_SOCK" "$@"