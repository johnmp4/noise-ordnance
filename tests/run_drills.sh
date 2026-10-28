#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance test suite: build validation, not an audio-engineering drill.
#
# What it does:
#   1. Derives the noisy drill files from the clean speech file if they
#      don't exist yet (derive-drills.py, deterministic, seed 42). Only the
#      clean file ships in the repo, which keeps large files out of it and
#      lets contributors run the tests against their own clean speech.
#   2. Builds tests/denoise_wav (offline harness around the REAL
#      dsp/DenoiserEngine.cpp -- no DPF, no GUI, no audio device).
#   3. Runs the drills through it in DAW-like blocks.
#   4. Scores each with tests/measure.py using the same windowed-RMS method
#      that validated the RNNoise model itself:
#        noise floor  = mean RMS of the quietest 5% of 20 ms windows
#        speech peaks = 99th percentile of window RMS
#      plus an exact sample-count check (catches the dropped-sample bug
#      class that starved the wet path in Oct 2026).
#
# Drill files live in tests/drills/ (16-bit PCM mono WAVs). Missing derived
# files are generated, not failed; a missing clean file means the affected
# drills SKIP.
#
# IMPORTANT: do not "test" this model with synthetic tones. A pure sine is
# tonal content, which RNNoise is designed to PRESERVE -- a tone measures
# "no reduction" even when the model is working perfectly. Use the drills.
# (Tones are fine for testing the hum filter in isolation, with the model
# out of the path -- that belongs in a unit test, not here.)
#
# Exit code: 0 if every gated drill passes, 1 otherwise.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRILLS="$ROOT/tests/drills"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD" "$DRILLS"

echo "=== Noise Ordnance test suite ==="

# --- derive noisy drills from clean audio if any are missing ---
# Existing derived files are left alone, so contributors can substitute
# their own clean speech: replace the clean file, delete the derived files,
# and re-run -- derive regenerates them from the new clean file.
DERIVED="paint-drying-hiss.wav paint-drying-hum.wav paint-drying-combo.wav paint-drying-quiet.wav"
NEED_DERIVE=0
for f in $DERIVED; do
  [[ -f "$DRILLS/$f" ]] || NEED_DERIVE=1
done
if [[ "$NEED_DERIVE" -eq 1 ]]; then
  if [[ -f "$DRILLS/paint-drying-clean.wav" ]]; then
    echo "--- deriving drill files from clean audio ---"
    python3 "$ROOT/tests/derive-drills.py" \
      --clean "$DRILLS/paint-drying-clean.wav" --out-dir "$DRILLS" \
      || echo "WARNING: derive failed; missing drills will SKIP"
  else
    echo "NOTE: clean audio missing, cannot derive drills; affected drills will SKIP"
  fi
fi

# --- build the harness (same third-party setup as the plugin Makefile) ---
SPEEXDSP_CFLAGS="$(pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")"
SPEEXDSP_LIBS="$(pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")"

echo "--- building denoise_wav ---"
# The RNNoise sources are C; everything else is C++. Compile them with the
# right compiler each (like the plugin Makefile does via DPF) -- g++ on the
# .c files fails on void* conversions.
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

PASS=0; FAIL=0; SKIP=0

# run_one <name> <file> <mode> <min_floor_db> <max_peak_db> <blocks> <hum> [note]
#   mode: gated      -> thresholds enforced, counts as PASS/FAIL
#         report     -> numbers printed only, with the given note
#   hum:  0 = hum notch off; 50/60 = hum notch module on at that mains freq
run_one() {
  local name="$1" file="$2" mode="$3" minfloor="$4" maxpeak="$5" blocks="$6"
  local hum="$7" note="${8:-}"
  local wav="$DRILLS/$file"

  if [[ ! -f "$wav" ]]; then
    echo "SKIP  $name: $file not found in tests/drills/"
    SKIP=$((SKIP + 1))
    return
  fi

  for b in $blocks; do
    local out="$BUILD/${name}-b${b}.wav"
    "$BUILD/denoise_wav" "$wav" "$out" "$b" 1.0 "$hum" >/dev/null
    echo "--- $name (block size $b, hum ${hum}) ---"
    if [[ "$mode" == "gated" ]]; then
      if python3 "$ROOT/tests/measure.py" "$wav" "$out" \
          --min-floor-reduction-db "$minfloor" --max-peak-delta-db "$maxpeak"; then
        echo "  PASS $name (block $b)"
        PASS=$((PASS + 1))
      else
        echo "  FAIL $name (block $b)"
        FAIL=$((FAIL + 1))
      fi
    else
      python3 "$ROOT/tests/measure.py" "$wav" "$out" --check-only
      echo "  (report-only: $note)"
    fi
  done
}

# Gated: the hiss drill is the v1 contract. Two block sizes because the
# Oct 2026 wet-path starvation bug was block-size sensitive.
run_one "paint-drying-hiss" "paint-drying-hiss.wav" gated 20 1.0 "128 1024" 0

# Gated: hum module regression. Threshold 40 dB is deliberately above what
# RNNoise alone achieves on this drill (~28 dB), so a broken/disabled hum
# module FAILS the gate; measured 88-110 dB with the module working, so
# 40 dB leaves wide headroom.
run_one "paint-drying-hum" "paint-drying-hum.wav" gated 40 1.0 "128 1024" 60

# Report-only guards: combo (hiss + hum) with the hum module on -- hum adds
# ~1 dB here, so no gate; the hum drill is the hum gate. Quiet is the
# no-speech guard: floor must not rise; listen to tests/build/paint-drying-quiet-b128.wav
# -- it should sound steady and quiet, not swelling/breathing (pumping) or chirpy artifacts.
run_one "paint-drying-combo" "paint-drying-combo.wav" report "" "" "128" 60 \
  "combo (hiss + hum) with the hum module on -- hum adds ~1 dB here, so no gate; the hum drill is the hum gate"
run_one "paint-drying-quiet" "paint-drying-quiet.wav" report "" "" "128" 0 \
  "no-speech guard: floor must not rise; listen to tests/build/paint-drying-quiet-b128.wav -- it should sound steady and quiet, not swelling/breathing (pumping) or chirpy artifacts"

echo "=== summary: $PASS passed, $FAIL failed, $SKIP skipped ==="
[[ "$FAIL" -eq 0 ]]
