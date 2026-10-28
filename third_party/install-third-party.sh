#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — third-party dependency installer.
#
# Downloads and patches:
#   - RNNoise (xiph/rnnoise) + Noise Ordnance patch (attenuation floor + gain smoothing)
#   - DPF (DISTRHO Plugin Framework)
#
# SpeexDSP is expected from the system (pkg-config speexdsp).
#   Ubuntu/Debian: sudo apt install libspeexdsp-dev
#
# Usage: bash third_party/install-third-party.sh
# Run from the project root.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TP="$ROOT/third_party"

# Pinned rnnoise commit — the Noise Ordnance patch is verified against this.
# xiph/rnnoise master is essentially frozen (no changes since Feb 2025),
# so pinning is safe and makes patching deterministic.
RNNOISE_PIN="70f1d256acd4b34a572f999a05c87bf00b67730d"

echo "=== Noise Ordnance: installing third-party dependencies ==="

# --- RNNoise ---
if [ -d "$TP/rnnoise/.git" ]; then
  echo "--- rnnoise: already cloned, resetting to pinned commit ---"
  (cd "$TP/rnnoise" && git fetch origin && git reset --hard "$RNNOISE_PIN")
else
  echo "--- rnnoise: cloning xiph/rnnoise ---"
  rm -rf "$TP/rnnoise"
  git clone https://github.com/xiph/rnnoise.git "$TP/rnnoise"
  (cd "$TP/rnnoise" && git reset --hard "$RNNOISE_PIN")
fi

echo "--- rnnoise: downloading model data ---"
(cd "$TP/rnnoise" && bash download_model.sh)
# Extract the model C files (rnnoise_data.c/h) into src/
model_tar=$(ls "$TP/rnnoise"/rnnoise_data-*.tar.gz 2>/dev/null | head -1)
if [ -n "$model_tar" ]; then
  tar -xzf "$model_tar" -C "$TP/rnnoise/src/" --strip-components=1 2>/dev/null || \
  tar -xzf "$model_tar" -C "$TP/rnnoise/src/" 2>/dev/null || \
  echo "  (could not extract model tarball - check manually)"
fi

echo "--- rnnoise: applying Noise Ordnance patch (attenuation floor + gain smoothing + alpha + band floor + VAD adapt) ---"
(cd "$TP/rnnoise" && \
  patch -p1 --forward < "$TP/rnnoise-noise-ordnance.patch" || \
  echo "  (patch already applied or failed - check manually)")

# Remove dev/training tools that break wildcard builds
rm -f "$TP/rnnoise/src/dump_features.c" \
      "$TP/rnnoise/src/dump_rnnoise_tables.c" \
      "$TP/rnnoise/src/write_weights.c" \
      "$TP/rnnoise/src/rnnoise_data_little.c"

# --- DPF ---
if [ -d "$TP/dpf/.git" ]; then
  echo "--- dpf: already cloned, updating ---"
  (cd "$TP/dpf" && git pull --ff-only)
else
  echo "--- dpf: cloning DISTRHO/DPF ---"
  rm -rf "$TP/dpf"
  git clone https://github.com/DISTRHO/DPF.git "$TP/dpf"
fi

# --- SpeexDSP check ---
echo "--- checking speexdsp ---"
if pkg-config --exists speexdsp; then
  echo "  speexdsp found: $(pkg-config --modversion speexdsp)"
else
  echo "  WARNING: speexdsp not found via pkg-config."
  echo "  Install it: sudo apt install libspeexdsp-dev  (Ubuntu/Debian)"
  echo "  The build will fail without it."
fi

echo ""
echo "=== Done ==="
echo "RNNoise: $TP/rnnoise (patched)"
echo "DPF:     $TP/dpf"
echo ""
echo "If the rnnoise patch reported failures, check the .rej files in"
echo "$TP/rnnoise/src/ -- the tree is pinned to a known-good commit,"
echo "so failures would indicate a local modification."
