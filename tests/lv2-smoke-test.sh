#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# LV2 bundle smoke test: is the built Noise Ordnance bundle well-formed and
# discoverable? This is structure validation, not a host load test:
#   1. the bundle directory exists, with manifest.ttl and the .so
#   2. manifest.ttl names the plugin URI, the binary, and the plugin TTL
#   3. the plugin TTL exists and declares the plugin (lv2:Plugin, same URI)
#   4. if lv2ls is installed, the plugin is discoverable through it
#   5. the bundle carries both DeepFilterNet model tarballs under
#      models/ -- a bundle without models runs dry (the engine passes
#      audio through), which is an incomplete build, not a working one.
#      The build scripts copy them in after TTL generation; this check
#      catches a build that skipped that step.
# Bundle location: $LV2_BUNDLE, else third_party/bin/NoiseOrdnance.lv2
# (the build output), else ~/.lv2/NoiseOrdnance.lv2 (the installed copy).
# A missing bundle FAILS -- build first (make + TTL generation), don't
# let this silently pass in a checkout that was never built. lv2ls is the
# only optional part: without it, structure validation is the verdict.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URI="https://plugins.johnpeterman.me/noise-ordnance"

BUNDLE="${LV2_BUNDLE:-}"
if [[ -z "$BUNDLE" ]]; then
  if [[ -d "$ROOT/third_party/bin/NoiseOrdnance.lv2" ]]; then
    BUNDLE="$ROOT/third_party/bin/NoiseOrdnance.lv2"
  elif [[ -d "$HOME/.lv2/NoiseOrdnance.lv2" ]]; then
    BUNDLE="$HOME/.lv2/NoiseOrdnance.lv2"
  fi
fi

fail() { echo "FAIL lv2-smoke: $1"; exit 1; }

[[ -n "$BUNDLE" && -d "$BUNDLE" ]] || fail "no LV2 bundle found (build first, or set LV2_BUNDLE)"
[[ -f "$BUNDLE/manifest.ttl" ]] || fail "$BUNDLE has no manifest.ttl"
[[ -f "$BUNDLE/NoiseOrdnance_dsp.so" ]] || fail "$BUNDLE has no NoiseOrdnance_dsp.so"

MANIFEST="$BUNDLE/manifest.ttl"
grep -qF "<$URI>" "$MANIFEST" || fail "manifest.ttl does not name $URI"
grep -qF "<NoiseOrdnance_dsp.so>" "$MANIFEST" || fail "manifest.ttl does not name the binary"
TTL_NAME="$(grep -o 'rdfs:seeAlso <[^>]*>' "$MANIFEST" | sed 's/.*<//;s/>//' | head -1)"
[[ -n "$TTL_NAME" ]] || fail "manifest.ttl has no rdfs:seeAlso plugin TTL"
[[ -f "$BUNDLE/$TTL_NAME" ]] || fail "manifest.ttl points at $TTL_NAME, which is missing"

TTL="$BUNDLE/$TTL_NAME"
grep -q "a .*lv2:Plugin" "$TTL" || fail "$TTL_NAME does not declare an lv2:Plugin"
grep -qF "<$URI>" "$TTL" || fail "$TTL_NAME does not contain the plugin URI"
grep -q "doap:name \"Noise Ordnance\"" "$TTL" || fail "$TTL_NAME does not name the plugin"

echo "--- lv2-smoke: bundle structure OK ($BUNDLE)"

for model in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  [[ -f "$BUNDLE/models/$model" ]] || fail "bundle is missing models/$model (a bundle without models runs dry)"
done
echo "--- lv2-smoke: both model tarballs present in models/"

if command -v lv2ls >/dev/null 2>&1; then
  if lv2ls 2>/dev/null | grep -qF "$URI"; then
    echo "--- lv2-smoke: discoverable via lv2ls"
  else
    fail "lv2ls is installed but does not list $URI (bundle not discoverable)"
  fi
else
  echo "--- lv2-smoke: lv2ls not installed, discoverability check skipped"
fi

echo "PASS lv2-smoke: $BUNDLE is well-formed"
