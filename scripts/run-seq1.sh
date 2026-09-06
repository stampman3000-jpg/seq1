#!/usr/bin/env bash
# Soundboy Pi launcher: correct cwd + DISPLAY for menu / autostart / SSH.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/seq1"

if [[ ! -x "$BIN" ]]; then
    echo "seq1 binary not found at $BIN" >&2
    echo "Build first: cmake -S \"$ROOT\" -B \"$ROOT/build\" && cmake --build \"$ROOT/build\" --parallel" >&2
    exit 1
fi

# Desktop menu / some launchers omit DISPLAY. Prefer the running local session.
if [[ -z "${DISPLAY:-}" ]]; then
    if [[ -S /tmp/.X11-unix/X0 ]]; then
        export DISPLAY=:0
    elif [[ -S /tmp/.X11-unix/X1 ]]; then
        export DISPLAY=:1
    else
        echo "No DISPLAY set and no local X socket found under /tmp/.X11-unix." >&2
        exit 1
    fi
fi

# Projects/patterns/presets/samples are cwd-relative.
cd "$ROOT"
exec "$BIN" "$@"
