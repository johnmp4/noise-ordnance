#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Run every engine unit test: build, then run each tests/build/unit-test-*
# binary and report PASS/FAIL counts. Exit 0 only if every test passes.
# A build failure fails the run (it must not silently report 0/0), and
# finding no test binaries at all is also a failure, not a pass.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/tests/build"

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
