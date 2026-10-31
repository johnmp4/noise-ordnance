#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Run every engine unit test: build, then run each tests/build/unit-test-*
# binary and report PASS/FAIL counts. Exit 0 only if every test passes.
# A build failure fails the run (it must not silently report 0/0), and
# finding no test binaries at all is also a failure, not a pass.
#
# Model files are required to RUN: the engine passes dry audio through
# when no model loads, so a missing model directory must fail here,
# before any test binary runs, instead of letting every test pass on
# dry audio. $NOISE_ORDNANCE_MODEL_DIR overrides the default
# (third_party/models).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/tests/build"

MODEL_DIR="${NOISE_ORDNANCE_MODEL_DIR:-$ROOT/third_party/models}"
for f in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  if [[ ! -f "$MODEL_DIR/$f" ]]; then
    echo "FAIL: model file missing: $MODEL_DIR/$f"
    echo "  Run: bash third_party/download-models.sh"
    echo "  Or set NOISE_ORDNANCE_MODEL_DIR to a directory holding both tarballs."
    exit 1
  fi
done
export NOISE_ORDNANCE_MODEL_DIR="$MODEL_DIR"
# Speech file for the latency unit test (correlation probe).
export NOISE_ORDNANCE_DRILLS_DIR="$ROOT/tests/drills"

bash "$ROOT/tests/build-unit-tests.sh"

PASS=0; FAIL=0; RAN=0
shopt -s nullglob
for bin in "$BUILD"/unit-test-*; do
  RAN=$((RAN + 1))
  if "$bin"; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
  fi
done

echo "=== unit tests: $PASS passed, $FAIL failed ==="
[[ "$RAN" -gt 0 && "$FAIL" -eq 0 ]]
