#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — build libDF (DeepFilterNet's Rust core) as a static
# library for the C++ plugin to link against.
#
# Prerequisite: a Rust toolchain. install-third-party.sh provides
# one: it uses an existing cargo if available, else installs a
# project-local rustup toolchain under third_party/rust/ (no shell
# profile changes). If you run this script directly, the same
# resolution applies below: PATH, then an existing rustup home, then
# the project-local toolchain (which needs CARGO_HOME/RUSTUP_HOME
# pointed at it, or the rustup cargo proxy cannot find its
# toolchain). No Rust code lives in this repo: the plugin calls
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

# Cargo resolution (mirrors install-third-party.sh): prefer an existing
# toolchain (PATH, then an existing rustup home), else the
# project-local toolchain under third_party/rust/. When the
# project-local toolchain is used, CARGO_HOME/RUSTUP_HOME must point
# at it, or the rustup cargo proxy cannot find its toolchain.
if ! command -v cargo >/dev/null 2>&1; then
  if [[ -x "${CARGO_HOME:-$HOME/.cargo}/bin/cargo" ]]; then
    export PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
  elif [[ -x "$ROOT/third_party/rust/cargo/bin/cargo" ]]; then
    export PATH="$ROOT/third_party/rust/cargo/bin:$PATH"
    export CARGO_HOME="$ROOT/third_party/rust/cargo"
    export RUSTUP_HOME="$ROOT/third_party/rust/rustup"
  else
    echo "FAIL: cargo not found. Run third_party/install-third-party.sh"
    echo "  (it installs a project-local Rust toolchain) or install Rust"
    echo "  via rustup: https://rustup.rs and make sure cargo is on PATH."
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
