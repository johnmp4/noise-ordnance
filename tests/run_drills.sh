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
#   2. Builds tests/build/denoise_wav (offline harness around the REAL
#      dsp/DenoiserEngine.cpp -- no DPF, no GUI, no audio device).
#   3. Runs the drills through it in DAW-like blocks.
#   4. Scores each with tests/measure.py using windowed RMS.
# The pumping check (tests/pumping-check.py) and harness settings test
# (tests/test-harness-settings.sh) run separately in validate-build.sh.
#
# Gates: hiss >= 20 dB floor reduction, peaks within 1 dB; hum >= 40 dB
# floor reduction (the hum drill gates the reusable notch bank: the
# 40 dB bar is set so a broken hum module fails), peaks within 1 dB.
# Blocks 128 and 1024.
# Combo (hiss + hum) and quiet (room tone, no speech) are report-only:
# combo measures the full chain as a sanity figure, quiet is a no-speech
# guard (the floor must not rise).
#
# The DeepFilterNet core is a speech model, so drill speech is a valid
# test signal for it. A synthetic tone is NOT: DF's gains are learned on
# speech spectra, and a tone through it measures nothing meaningful.
# Drills run on DeepFilterNet3 (model 0) -- the default model.
# DF crushes the hiss floor to near-silence, so the 20 dB hiss gate
# passes with huge margin; the gates are kept because a broken harness
# or a missing model would still fail them (denoise_wav exits 1 when no
# model loads, and this script does not tolerate that).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$ROOT/tests/drills"
OUT="$ROOT/tests/drills/out"
mkdir -p "$OUT"

# --- models: required to run (a missing model must fail, not pass dry) ---
MODEL_DIR="${NOISE_ORDNANCE_MODEL_DIR:-$ROOT/third_party/models}"
for f in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  if [[ ! -f "$MODEL_DIR/$f" ]]; then
    echo "FAIL: model file missing: $MODEL_DIR/$f"
    echo "  Run: bash third_party/download-models.sh"
    exit 1
  fi
done
export NOISE_ORDNANCE_MODEL_DIR="$MODEL_DIR"

# --- derive noisy variants if any are missing ---
CLEAN="$DIR/paint-drying-clean.wav"
if [[ ! -f "$CLEAN" ]]; then
  echo "SKIP: clean drill file missing ($CLEAN); drills need it."
  exit 0
fi
for f in paint-drying-hiss.wav paint-drying-hum.wav paint-drying-combo.wav paint-drying-quiet.wav; do
  if [[ ! -f "$DIR/$f" ]]; then
    echo "--- deriving drill files (deterministic, seed 42) ---"
    python3 "$ROOT/tests/derive-drills.py" --clean "$CLEAN" --out-dir "$DIR"
    break
  fi
done

# --- build denoise_wav ---
DF_LIB="$ROOT/third_party/deepfilternet/target/release/libdf.a"
if [[ ! -f "$DF_LIB" ]]; then
  echo "FAIL: libDF static library missing: $DF_LIB"
  echo "  Run: bash third_party/build-libdf.sh  (or third_party/install-third-party.sh)"
  exit 1
fi
SPEEXDSP_CFLAGS="$(pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")"
SPEEXDSP_LIBS="$(pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")"
CXX="${CXX:-g++}"
echo "--- building denoise_wav ---"
mkdir -p "$ROOT/tests/build"
# shellcheck disable=SC2086
"$CXX" -O2 -std=c++17 \
  -I"$ROOT" -I"$ROOT/dsp" \
  $SPEEXDSP_CFLAGS \
  "$ROOT/tests/denoise_wav.cpp" "$ROOT/dsp/DenoiserEngine.cpp" \
  -o "$ROOT/tests/build/denoise_wav" -lm $SPEEXDSP_LIBS -lpthread -ldl "$DF_LIB"

BIN="$ROOT/tests/build/denoise_wav"
MEASURE="python3 $ROOT/tests/measure.py"

# $1 = drill base name, $2 = gates ("hiss"|"hum"|"report"), $3 = extra args
# Detail goes to stderr; only the "pass fail" counts go to stdout, so
# callers can capture them with: read -r p f < <(run_drill ...)
run_drill() {
  local name="$1" gates="$2"; shift 2
  local src="$DIR/paint-drying-$name.wav"
  local g_pass=0 g_fail=0
  for block in 128 1024; do
    local dst="$OUT/paint-drying-$name-$block.wav"
    "$BIN" "$src" "$dst" "$block" 1.0 "$@" >/dev/null 2>"$OUT/$name-$block.log" \
      || { echo "FAIL $name block $block: denoise_wav exited nonzero (see $OUT/$name-$block.log)" >&2; g_fail=$((g_fail+1)); continue; }
    if [[ "$gates" == "report" ]]; then
      echo "--- $name block $block (report-only) ---" >&2
      $MEASURE --check-only "$src" "$dst" >&2 || true
    else
      echo "--- $name block $block ---" >&2
      if $MEASURE "$src" "$dst" --min-floor-reduction-db "${FLOOR_GATE:-20}" --max-peak-delta-db 1.0 >&2; then
        g_pass=$((g_pass+1))
      else
        g_fail=$((g_fail+1))
      fi
    fi
  done
  echo "$g_pass $g_fail"
}

TOTAL_PASS=0; TOTAL_FAIL=0

# hiss: DF model, no hum module (gates 20 dB / 1 dB)
read -r p f < <(run_drill hiss hiss)
TOTAL_PASS=$((TOTAL_PASS+p)); TOTAL_FAIL=$((TOTAL_FAIL+f))

# hum: notch module on at 60 Hz, gates 40 dB / 1 dB
read -r p f < <(FLOOR_GATE=40 run_drill hum hum 60)
TOTAL_PASS=$((TOTAL_PASS+p)); TOTAL_FAIL=$((TOTAL_FAIL+f))

# combo: full chain (DF + hum at 60 Hz), report-only
read -r p f < <(run_drill combo report 60)
# quiet: no-speech guard, report-only
read -r p f < <(run_drill quiet report)

echo "=== drills: $TOTAL_PASS passed, $TOTAL_FAIL failed ==="
if [[ "$TOTAL_FAIL" -eq 0 ]]; then exit 0; else exit 1; fi
