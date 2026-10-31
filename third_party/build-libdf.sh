#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — build libDF (DeepFilterNet's Rust core) as a static
# library for the C++ plugin to link against.
#
# Prerequisite: a Rust toolchain. Install via rustup (https://rustup.rs):
#   curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
# then make sure cargo is on PATH (rustup's installer usually does this
# via ~/.cargo/env). No Rust code lives in this repo: the plugin calls
# libDF through the plain C API declared in dsp/df.h.
#
# What gets built: third_party/deepfilternet/libDF with the `capi`
# feature (C API + tract ONNX runtime). CARGO_TARGET_DIR is pinned so
# the Makefile can rely on third_party/deepfilternet/target/release/
# libdf.a regardless of cargo workspace layout. The `capi` feature also
# pulls in libDF's embedded default model (~8 MB of bytes the plugin
# never uses -- models load from files -- see BRANCH-NOTES.md); that is
# accepted bloat for this branch, not a requirement. (libDF main
# declares the staticlib crate-type already, so plain `cargo build`
# produces libdf.a; no tree patching needed.)
#
# First build downloads crates from crates.io (network required) and
# takes a while (tract is a large dependency tree); later builds are
# incremental. Disk: allow ~2 GB for the cargo registry + target dir.
#
# Usage: bash third_party/build-libdf.sh
# Run from the project root (or anywhere: it resolves its own dir).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DF="$ROOT/third_party/deepfilternet"

if [[ ! -d "$DF/libDF" ]]; then
  echo "FAIL: $DF/libDF not found -- run third_party/install-third-party.sh first"
  exit 1
fi

if ! command -v cargo >/dev/null 2>&1; then
  # rustup's usual location, in case PATH wasn't updated in this shell.
  if [[ -x "$HOME/.cargo/bin/cargo" ]]; then
    export PATH="$HOME/.cargo/bin:$PATH"
  else
    echo "FAIL: cargo not found. Install Rust via rustup: https://rustup.rs"
    echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    exit 1
  fi
fi

echo "=== Noise Ordnance: building libDF (staticlib, capi feature) ==="
echo "    source: $DF (pinned commit in install-third-party.sh)"
(
  cd "$DF"
  CARGO_TARGET_DIR="$DF/target" \
    cargo build --release \
      --manifest-path libDF/Cargo.toml \
      --no-default-features --features capi
)

LIB="$DF/target/release/libdf.a"
if [[ -f "$LIB" ]]; then
  echo "=== libDF built: $LIB ($(du -h "$LIB" | cut -f1)) ==="
else
  echo "FAIL: expected static library missing: $LIB"
  exit 1
fi
