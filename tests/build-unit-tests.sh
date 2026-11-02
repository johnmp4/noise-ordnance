#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Build the engine unit tests (tests/unit/test-*.cpp) against the REAL
# dsp/DenoiserEngine.cpp -- no DPF, no GUI, no audio device.
# Each test becomes tests/build/unit-test-<name>. New tests are picked up
# automatically by globbing tests/unit/test-*.cpp, so adding a test is just
# adding one file (and one commit, per the repo's one-test-per-commit rule).
#
# The engine links libDF (Rust static library): build it first with
# third_party/build-libdf.sh (the install script runs it). A missing
# static library fails here with a pointer, instead of a confusing link
# error. Model files are NOT needed to build -- only to run, which is
# run-unit-tests.sh's check.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/tests/build"
mkdir -p "$BUILD"

# Remove stale test binaries first: a test file that was deleted must
# not keep running from a previous build (a stale binary would fail
# the suite for a test that no longer exists -- or worse, pass it).
rm -f "$BUILD"/unit-test-*

DF_LIB="$ROOT/third_party/deepfilternet/target/release/libdf.a"
if [[ ! -f "$DF_LIB" ]]; then
  echo "FAIL: libDF static library missing: $DF_LIB"
  echo "  Run: bash third_party/build-libdf.sh  (or third_party/install-third-party.sh)"
  exit 1
fi

SPEEXDSP_CFLAGS="$(pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")"
SPEEXDSP_LIBS="$(pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")"
CXX="${CXX:-g++}"

for test_src in "$ROOT"/tests/unit/test-*.cpp; do
  name="$(basename "$test_src" .cpp)"
  bin="$BUILD/unit-${name}"
  echo "--- building $bin ---"
  # shellcheck disable=SC2086
  "$CXX" -O2 -std=c++17 \
    -I"$ROOT" -I"$ROOT/dsp" -I"$ROOT/tests/unit" \
    $SPEEXDSP_CFLAGS \
    "$test_src" "$ROOT/dsp/DenoiserEngine.cpp" \
    -o "$bin" -lm $SPEEXDSP_LIBS -lpthread -ldl "$DF_LIB"
done

echo "unit tests built in $BUILD"
