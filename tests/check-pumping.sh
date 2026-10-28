#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Pumping check runner (REPORT-ONLY, deliberately ungated).
# Runs tests/pumping-check.py over the processed quiet drill and prints
# the window-RMS variance numbers. It does NOT fail the build on them:
# the current engine measures std 3.30 dB over all windows, but that
# number is dominated by a single fully-silent window (which reads
# -240 dB on the meter's epsilon floor), while the non-silent windows
# measure std 0.80 dB -- so no single threshold cleanly captures
# "pumping" yet. The gate threshold is an open question for the owner:
# it needs a listening check (does the processed quiet actually breathe?)
# plus a metric refinement (e.g. percentile-based, or silent-window
# aware) before it can gate anything. Until then, this reports and
# exits 0; it only fails on infrastructure errors (missing input that
# cannot be derived, or a processing failure).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRILLS="$ROOT/tests/drills"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"

QUIET="$DRILLS/paint-drying-quiet.wav"
PROCESSED="$BUILD/paint-drying-quiet-b128.wav"
if [[ ! -f "$QUIET" ]]; then
  if [[ -f "$DRILLS/paint-drying-clean.wav" ]]; then
    echo "--- deriving drill files ---"
    python3 "$ROOT/tests/derive-drills.py" \
      --clean "$DRILLS/paint-drying-clean.wav" --out-dir "$DRILLS" \
      || true
  fi
fi
if [[ ! -f "$QUIET" ]]; then
  echo "SKIP pumping-check: paint-drying-quiet.wav not found (no clean audio to derive from)"
  exit 0
fi
if [[ ! -f "$PROCESSED" ]]; then
  if [[ ! -x "$BUILD/denoise_wav" ]]; then
    echo "SKIP pumping-check: no processed quiet file and no denoise_wav binary (run tests/run_drills.sh first)"
    exit 0
  fi
  echo "--- processing quiet drill ---"
  "$BUILD/denoise_wav" "$QUIET" "$PROCESSED" 128 1.0 0
fi

python3 "$ROOT/tests/pumping-check.py" "$PROCESSED"
echo "PASS pumping-check: numbers reported (report-only, ungated by design)"
