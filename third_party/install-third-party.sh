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
# A Rust toolchain is required for the libDF build. Toolchain policy
# (deliberate, contributor-facing): nothing is ever installed outside
# this project tree. An existing cargo on PATH (or in an existing
# rustup home) is used as-is; otherwise a project-local rustup
# toolchain is installed under third_party/rust/ (see below).
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
# Toolchain policy (deliberate, contributor-facing): nothing is ever
# installed outside this project tree. If cargo is already available
# (PATH, or an existing rustup home), it is used as-is. Otherwise a
# project-local rustup toolchain is installed under third_party/rust/
# with --no-modify-path, so no shell profile is touched and nothing
# lands in $HOME. The install is announced loudly below but needs no
# prompt: like the DPF clone and model downloads, it only writes
# inside this project. --profile minimal keeps it to rustc/cargo/std.
# The post-install check catches a failed download (curl | sh without
# pipefail can otherwise look successful) and aborts before anything
# else is fetched.
RUST_DIR="$TP/rust"
if ! command -v cargo >/dev/null 2>&1; then
  if [[ -x "${CARGO_HOME:-$HOME/.cargo}/bin/cargo" ]]; then
    echo "--- cargo found at ${CARGO_HOME:-$HOME/.cargo}/bin, adding to PATH ---"
    export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
  elif [[ -x "$RUST_DIR/cargo/bin/cargo" ]]; then
    echo "--- cargo found at project-local $RUST_DIR/cargo/bin, adding to PATH ---"
    export PATH="$RUST_DIR/cargo/bin:$PATH"
    export CARGO_HOME="$RUST_DIR/cargo" RUSTUP_HOME="$RUST_DIR/rustup"
  else
    echo "--- Rust toolchain not found; installing project-local rustup ---"
    echo "  location: $RUST_DIR (inside this project, gitignored)"
    echo "  no shell profile changes, nothing installed in \$HOME"
    echo "  profile: minimal (rustc/cargo/std), downloads a few hundred MB"
    export CARGO_HOME="$RUST_DIR/cargo" RUSTUP_HOME="$RUST_DIR/rustup"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --profile minimal
    # The installer does not touch PATH (--no-modify-path), so add the
    # project-local bin dir explicitly -- otherwise the check below
    # fails even though the toolchain just installed successfully.
    export PATH="$RUST_DIR/cargo/bin:$PATH"
  fi
fi
if ! command -v cargo >/dev/null 2>&1; then
  echo "FAIL: cargo still not available after rustup install attempt."
  echo "  Run third_party/install-third-party.sh again, or install Rust"
  echo "  manually and make sure cargo is on PATH."
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
