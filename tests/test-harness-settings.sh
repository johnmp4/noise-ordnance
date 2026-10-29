#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Harness settings test: proves tests/denoise_wav actually honors its
# smoothing/floor/VAD arguments, instead of silently ignoring them.
#   1. Runs the hiss drill twice: once with defaults, once with
#      non-default settings (smoothing 0.3, floors -18, VAD adapt on).
#   2. The two outputs must differ -- if the args were ignored, the
#      outputs would be identical and this fails.
#   3. The settings run must still denoise sanely (same gates as the hiss
#      drill: >= 20 dB floor reduction, peaks within 1 dB).
# Sample counts must match the input exactly in both runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRILLS="$ROOT/tests/drills"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"

HISS="$DRILLS/paint-drying-hiss.wav"
if [[ ! -f "$HISS" ]]; then
  if [[ -f "$DRILLS/paint-drying-clean.wav" ]]; then
    echo "--- deriving drill files ---"
    python3 "$ROOT/tests/derive-drills.py" \
      --clean "$DRILLS/paint-drying-clean.wav" --out-dir "$DRILLS" \
      || true
  fi
fi
if [[ ! -f "$HISS" ]]; then
  echo "SKIP harness-settings: paint-drying-hiss.wav not found (no clean audio to derive from)"
  exit 0
fi

# Build the harness (same flags as run_drills.sh).
SPEEXDSP_CFLAGS="$(pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")"
SPEEXDSP_LIBS="$(pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")"
CC="${CC:-cc}"
CXX="${CXX:-g++}"
shopt -s nullglob
C_OBJS=()
for src in "$ROOT"/third_party/rnnoise/src/*.c; do
  obj="$BUILD/$(basename "$src" .c).o"
  # shellcheck disable=SC2086
  "$CC" -O2 \
    -I"$ROOT/third_party/rnnoise/include" -I"$ROOT/third_party/rnnoise/src" \
    -DFLOAT_APPROX \
    $SPEEXDSP_CFLAGS \
    -c "$src" -o "$obj"
  C_OBJS+=("$obj")
done
# shellcheck disable=SC2086
"$CXX" -O2 -std=c++17 \
  -I"$ROOT" -I"$ROOT/dsp" \
  -I"$ROOT/third_party/rnnoise/include" -I"$ROOT/third_party/rnnoise/src" \
  -DFLOAT_APPROX \
  $SPEEXDSP_CFLAGS \
  "$ROOT/tests/denoise_wav.cpp" "$ROOT/dsp/DenoiserEngine.cpp" \
  "${C_OBJS[@]}" \
  -o "$BUILD/denoise_wav" -lm $SPEEXDSP_LIBS

DEFAULT_OUT="$BUILD/harness-settings-default.wav"
SETTINGS_OUT="$BUILD/harness-settings-custom.wav"
"$BUILD/denoise_wav" "$HISS" "$DEFAULT_OUT" 128 1.0 0
"$BUILD/denoise_wav" "$HISS" "$SETTINGS_OUT" 128 1.0 0 0.3 -18 -18 -18 1

python3 - "$HISS" "$DEFAULT_OUT" "$SETTINGS_OUT" <<'EOF'
import struct
import sys
import wave

def read(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    raw = w.readframes(n)
    w.close()
    return struct.unpack(f"<{n}h", raw)

orig, default, custom = (read(p) for p in sys.argv[1:4])
assert len(default) == len(orig), f"default count {len(default)} != input {len(orig)}"
assert len(custom) == len(orig), f"settings count {len(custom)} != input {len(orig)}"
maxdiff = max(abs(a - b) for a, b in zip(default, custom))
print(f"--- harness-settings: counts {len(orig)} == {len(default)} == {len(custom)}, "
      f"default-vs-settings max diff {maxdiff} LSB")
assert maxdiff > 0, "settings run is identical to the default run -- args ignored?"
EOF

python3 "$ROOT/tests/measure.py" "$HISS" "$SETTINGS_OUT" \
  --min-floor-reduction-db 20 --max-peak-delta-db 1.0

echo "PASS harness-settings: smoothing/floor/VAD args take effect and denoise sanely"
