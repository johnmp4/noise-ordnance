#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Build the engine unit tests (tests/unit/test-*.cpp) against the REAL
# dsp/DenoiserEngine.cpp -- no DPF, no GUI, no audio device.
# Each test becomes tests/build/unit-test-<name>. New tests are picked up
# automatically by globbing tests/unit/test-*.cpp, so adding a test is just
# adding one file (and one commit, per the repo's one-test-per-commit rule).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"

SPEEXDSP_CFLAGS="$(pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")"
SPEEXDSP_LIBS="$(pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")"
CC="${CC:-cc}"
CXX="${CXX:-g++}"

# RNNoise sources are C; everything else is C++ (same split as run_drills.sh).
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

for test_src in "$ROOT"/tests/unit/test-*.cpp; do
  name="$(basename "$test_src" .cpp)"
  bin="$BUILD/unit-${name}"
  echo "--- building $bin ---"
  # shellcheck disable=SC2086
  "$CXX" -O2 -std=c++17 \
    -I"$ROOT" -I"$ROOT/dsp" -I"$ROOT/tests/unit" \
    -I"$ROOT/third_party/rnnoise/include" -I"$ROOT/third_party/rnnoise/src" \
    -DFLOAT_APPROX \
    $SPEEXDSP_CFLAGS \
    "$test_src" "$ROOT/dsp/DenoiserEngine.cpp" \
    "${C_OBJS[@]}" \
    -o "$bin" -lm $SPEEXDSP_LIBS
done

echo "unit tests built in $BUILD"
