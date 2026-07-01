#!/usr/bin/env bash
# shot.sh — headless offscreen render to a PNG, for visual iteration without a
# display. Renders via SDL's offscreen (EGL surfaceless) driver on the GPU, so
# nothing appears on the desktop.
#
# Usage:
#   tools/shot.sh OUT.png [verse args...]
#
# Examples:
#   tools/shot.sh /tmp/bh.png --preset assets/universes/black_hole.json \
#                 --cam 0,0.25,0.85,-90,-16 --frames 12
#
# Requires: ./verse built (make IMGUI=1), ffmpeg on PATH.
set -euo pipefail

out="${1:?usage: shot.sh OUT.png [verse args...]}"; shift
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp --suffix=.ppm)"

# --shot/--frames default if the caller didn't pass them.
args=("$@")
case " ${args[*]} " in *" --frames "*) ;; *) args+=(--frames 12);; esac

timeout 120 "$here/verse" --headless --shot "$tmp" "${args[@]}" >/dev/null 2>&1 || true
[ -s "$tmp" ] || { echo "render produced no frame" >&2; exit 1; }
ffmpeg -y -loglevel error -i "$tmp" "$out"
rm -f "$tmp"
echo "wrote $out"
