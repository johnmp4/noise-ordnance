#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# validate-build.sh: run the build plus the full test set, in order.
# This is the release build script: plain `make` builds every target
# (LV2 + VST3), the built bundles carry the models, and every gated step
# must pass -- a green run here produces shippable bundles. Non-interactive;
# install separately with install.sh.
#   1. Plugin build: make (LV2 + VST3) + LV2 TTL generation.
#   2. tests/run_drills.sh -- audio drills (gated hiss + hum, report-only
#      combo + quiet).
#   3. tests/run-unit-tests.sh -- engine unit tests.
#   4. tests/check-latency.py -- per-model latency vs the table, by
#      speech cross-correlation (exact; needs denoise_wav from step 2).
#   5. tests/lv2-smoke-test.sh -- LV2 bundle structure + discoverability
#      (Linux only; LV2 is not shipped on Windows).
#   6. tests/vst3-smoke-test.sh -- VST3 bundle structure (binary +
#      models) on every platform; VST3 is the shipped format on both.
#   7. tests/param-contract.py -- 10 parameters vs the README table, from
#      the freshly generated TTL, plus the Low Latency switch toggle pin.
#   8. tests/test-harness-settings.sh -- denoise_wav model/pfbeta/atten args.
#   9. tests/check-pumping.sh -- window-RMS variance on processed quiet
#      (report-only by design; see that script for why it has no gate).
# Exit 0 only if every gated step passes. Requires a full checkout: if
# third_party/dpf or the libDF static library or the model files are
# missing, run third_party/install-third-party.sh first instead of
# silently skipping the plugin build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

if [[ ! -d "$ROOT/third_party/dpf" ]]; then
  echo "FAIL validate-build: third_party/dpf missing -- run third_party/install-third-party.sh first"
  exit 1
fi
if [[ ! -f "$ROOT/third_party/deepfilternet/target/release/libdf.a" ]]; then
  echo "FAIL validate-build: libDF static library missing -- run third_party/build-libdf.sh (or install-third-party.sh) first"
  exit 1
fi
for model in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  if [[ ! -f "$ROOT/third_party/models/$model" ]]; then
    echo "FAIL validate-build: model file missing: third_party/models/$model -- run third_party/download-models.sh first"
    exit 1
  fi
done

echo "=== validate-build: plugin build ==="
make -j"$(nproc)"
if [ ! -x third_party/dpf/utils/lv2_ttl_generator ]; then
  cc third_party/dpf/utils/lv2-ttl-generator/lv2_ttl_generator.c -o third_party/dpf/utils/lv2_ttl_generator -ldl
fi
bash third_party/dpf/utils/generate-ttl.sh third_party/bin

# The plugin resolves models from $NOISE_ORDNANCE_MODEL_DIR, else
# <bundle>/models/. Bundles built here must carry the models, or the
# smoke test (and any host load) would run dry. VST3 uses the same
# <bundle>/models/ layout as a best-effort convention.
echo "=== validate-build: copying models into build bundles ==="
mkdir -p third_party/bin/NoiseOrdnance.lv2/models third_party/bin/NoiseOrdnance.vst3/models
cp third_party/models/DeepFilterNet3_onnx.tar.gz third_party/models/DeepFilterNet3_ll_onnx.tar.gz third_party/bin/NoiseOrdnance.lv2/models/
cp third_party/models/DeepFilterNet3_onnx.tar.gz third_party/models/DeepFilterNet3_ll_onnx.tar.gz third_party/bin/NoiseOrdnance.vst3/models/
# CLAP is a single file, not a bundle: the models live in a
# NoiseOrdnance/ folder next to the .clap file in the build output,
# which is where the plugin looks for them (see NoiseOrdnance.cpp).
mkdir -p third_party/bin/NoiseOrdnance/models
cp third_party/models/DeepFilterNet3_onnx.tar.gz third_party/models/DeepFilterNet3_ll_onnx.tar.gz third_party/bin/NoiseOrdnance/models/

echo "=== validate-build: audio drills ==="
bash tests/run_drills.sh

echo "=== validate-build: engine unit tests ==="
bash tests/run-unit-tests.sh

echo "=== validate-build: latency check (speech correlation) ==="
python3 tests/check-latency.py

echo "=== validate-build: LV2 smoke test ==="
if [[ "$(uname -s)" == Linux* ]]; then
  bash tests/lv2-smoke-test.sh
else
  echo "--- validate-build: LV2 smoke test skipped (LV2 is Linux-only in this project)"
fi

echo "=== validate-build: VST3 smoke test ==="
bash tests/vst3-smoke-test.sh

echo "=== validate-build: parameter contract ==="
python3 tests/param-contract.py \
  --ttl third_party/bin/NoiseOrdnance.lv2/NoiseOrdnance_dsp.ttl

echo "=== validate-build: harness settings ==="
bash tests/test-harness-settings.sh

echo "=== validate-build: pumping check (report-only) ==="
bash tests/check-pumping.sh

echo "=== validate-build: ALL GREEN ==="
