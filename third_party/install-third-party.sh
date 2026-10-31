#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — third-party dependency installer (DeepFilterNet
# engine branch).
#
# Fetches:
#   - DPF (DISTRHO Plugin Framework)
#   - DeepFilterNet (Rikorose/DeepFilterNet, pinned commit below) and
#     builds libDF as a static library (third_party/build-libdf.sh)
#   - The two DeepFilterNet model tarballs
#     (third_party/download-models.sh) -- models are NOT in git
#
# SpeexDSP is expected from the system (pkg-config speexdsp).
#   Ubuntu/Debian: sudo apt install libspeexdsp-dev
# A Rust toolchain is required for the libDF build. If cargo is not
# already available, this script installs it via rustup below
# (user-local, ~/.cargo; no sudo) and sources it for the rest of
# the run, so a fresh clone goes straight to a working build.
#
# Usage: bash third_party/install-third-party.sh
# Run from the project root.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TP="$ROOT/third_party"

# Pinned DeepFilterNet commit -- main at d375b2d (Oct 2024, the current
# main tip). The C API (libDF/src/capi.rs) this branch's dsp/df.h was
# written against, and the models/ tree the download script fetches
# from. Pinning keeps the build deterministic; bump it deliberately
# (and re-check capi.rs + re-measure latencies) if you move it.
#
# Pin history: this branch briefly tried v0.5.6 (tract 0.19.16) hoping
# it would load DeepFilterNet2, but DF2 fails to load under BOTH
# versions (tract reshape lowering panics; see BRANCH-NOTES.md), so
# there is no reason to carry v0.5.6's older tree: main builds on
# current stable Rust, declares the staticlib crate-type already, and
# gives identical DF3/DF3_ll behavior (same measured latencies).
DF_PIN="d375b2d8309e0935d165700c91da9de862a99c31"

echo "=== Noise Ordnance: installing third-party dependencies ==="

# --- Rust toolchain (required for the libDF build) ---
# Auto-install only when cargo is genuinely absent: check PATH first,
# then rustup's usual location, so an existing toolchain (system or
# rustup) is never disturbed. The installer appends cargo env sourcing
# to your shell profile; we source it here too so this run can use
# cargo immediately. The post-install check below catches a failed
# download (curl | sh without pipefail can otherwise look successful)
# and aborts before anything else is fetched.
if ! command -v cargo >/dev/null 2>&1; then
  if [[ -x "$HOME/.cargo/bin/cargo" ]]; then
    echo "--- cargo found at $HOME/.cargo/bin, adding to PATH ---"
    export PATH="$HOME/.cargo/bin:$PATH"
  else
    echo "--- cargo not found: installing Rust via rustup ---"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    # shellcheck disable=SC1091
    if [[ -f "${CARGO_HOME:-$HOME/.cargo}/env" ]]; then
      source "${CARGO_HOME:-$HOME/.cargo}/env"
    else
      export PATH="$HOME/.cargo/bin:$PATH"
    fi
  fi
fi
if ! command -v cargo >/dev/null 2>&1; then
  echo "FAIL: cargo still not available after rustup install attempt."
  echo "  Install Rust manually: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
  exit 1
fi

# --- DeepFilterNet ---
if [ -d "$TP/deepfilternet/.git" ]; then
  echo "--- deepfilternet: already cloned, resetting to pinned commit ---"
  (cd "$TP/deepfilternet" && git fetch origin && git reset --hard "$DF_PIN")
else
  echo "--- deepfilternet: cloning Rikorose/DeepFilterNet ---"
  rm -rf "$TP/deepfilternet"
  git clone https://github.com/Rikorose/DeepFilterNet.git "$TP/deepfilternet"
  (cd "$TP/deepfilternet" && git reset --hard "$DF_PIN")
fi

echo "--- deepfilternet: building libDF static library ---"
bash "$TP/build-libdf.sh"

echo "--- deepfilternet: downloading model files ---"
bash "$TP/download-models.sh"

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
echo "DeepFilterNet: $TP/deepfilternet (pinned $DF_PIN)"
echo "libDF:         $TP/deepfilternet/target/release/libdf.a"
echo "Models:        $TP/models/"
echo "DPF:           $TP/dpf"
