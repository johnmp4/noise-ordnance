#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# VST3 bundle smoke test: is the built Noise Ordnance bundle well-formed?
# This is structure validation, not a host load test:
#   1. the bundle directory exists, with the platform binary
#      (Linux: Contents/x86_64-linux/NoiseOrdnance.so;
#       Windows: Contents/x86_64-win/NoiseOrdnance.vst3)
#   2. the bundle carries both DeepFilterNet model tarballs under
#      models/ -- a bundle without models runs dry (the engine passes
#      audio through), which is an incomplete build, not a working one.
#      The build scripts copy them in after the build; this check
#      catches a build that skipped that step.
# Bundle location: $VST3_BUNDLE, else third_party/bin/NoiseOrdnance.vst3
# (the build output), else the installed copy (~/.vst3 on Linux,
# C:\Program Files\Common Files\VST3 on Windows).
# A missing bundle FAILS -- build first (make), don't let this silently
# pass in a checkout that was never built.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$(uname -s)" in
  Linux*)
    BIN="Contents/x86_64-linux/NoiseOrdnance.so"
    INSTALLED="$HOME/.vst3/NoiseOrdnance.vst3"
    ;;
  MINGW*|MSYS*)
    BIN="Contents/x86_64-win/NoiseOrdnance.vst3"
    INSTALLED="/c/Program Files/Common Files/VST3/NoiseOrdnance.vst3"
    ;;
  *)
    echo "FAIL vst3-smoke: unsupported OS '$(uname -s)' -- Linux and Windows (MSYS2/MinGW64) only."
    exit 1
    ;;
esac

BUNDLE="${VST3_BUNDLE:-}"
if [[ -z "$BUNDLE" ]]; then
  if [[ -d "$ROOT/third_party/bin/NoiseOrdnance.vst3" ]]; then
    BUNDLE="$ROOT/third_party/bin/NoiseOrdnance.vst3"
  elif [[ -d "$INSTALLED" ]]; then
    BUNDLE="$INSTALLED"
  fi
fi

fail() { echo "FAIL vst3-smoke: $1"; exit 1; }

[[ -n "$BUNDLE" && -d "$BUNDLE" ]] || fail "no VST3 bundle found (build first, or set VST3_BUNDLE)"
[[ -f "$BUNDLE/$BIN" ]] || fail "$BUNDLE has no $BIN"

echo "--- vst3-smoke: bundle structure OK ($BUNDLE)"

for model in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  [[ -f "$BUNDLE/models/$model" ]] || fail "bundle is missing models/$model (a bundle without models runs dry)"
done
echo "--- vst3-smoke: both model tarballs present in models/"

echo "PASS vst3-smoke: $BUNDLE is well-formed"
