#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# validate-build.sh: run the build plus the full test set, in order.
#   1. Plugin build: make (LV2 + VST3) + LV2 TTL generation. This mirrors
#      the build portion of build-test-install.sh, minus its interactive
#      install prompt -- validation should not block on a question.
#   2. tests/run_drills.sh -- audio drills (gated hiss + hum, report-only
#      combo + quiet).
#   3. tests/run-unit-tests.sh -- engine unit tests.
#   4. tests/lv2-smoke-test.sh -- LV2 bundle structure + discoverability.
#   5. tests/param-contract.py -- 12 parameters vs the README table, from
#      the freshly generated TTL.
#   6. tests/test-harness-settings.sh -- denoise_wav smoothing/floor/VAD args.
#   7. tests/check-pumping.sh -- window-RMS variance on processed quiet
#      (report-only by design; see that script for why it has no gate).
# Exit 0 only if every gated step passes. Requires a full checkout: if
# third_party/dpf is missing, run third_party/install-third-party.sh first
# instead of silently skipping the plugin build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ ! -d "$ROOT/third_party/dpf" ]]; then
  echo "FAIL validate-build: third_party/dpf missing -- run third_party/install-third-party.sh first"
  exit 1
fi

echo "=== validate-build: plugin build ==="
make -j"$(nproc)"
if [ ! -x third_party/dpf/utils/lv2_ttl_generator ]; then
  cc third_party/dpf/utils/lv2-ttl-generator/lv2_ttl_generator.c -o third_party/dpf/utils/lv2_ttl_generator -ldl
fi
bash third_party/dpf/utils/generate-ttl.sh third_party/bin

echo "=== validate-build: audio drills ==="
bash tests/run_drills.sh

echo "=== validate-build: engine unit tests ==="
bash tests/run-unit-tests.sh

echo "=== validate-build: LV2 smoke test ==="
bash tests/lv2-smoke-test.sh

echo "=== validate-build: parameter contract ==="
python3 tests/param-contract.py \
  --ttl third_party/bin/NoiseOrdnance.lv2/NoiseOrdnance_dsp.ttl

echo "=== validate-build: harness settings ==="
bash tests/test-harness-settings.sh

echo "=== validate-build: pumping check (report-only) ==="
bash tests/check-pumping.sh

echo "=== validate-build: ALL GREEN ==="
