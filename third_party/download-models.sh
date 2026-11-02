#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — download the DeepFilterNet model files.
#
# Model files are NOT committed to git (they are large binaries, and
# the project's pattern is that third_party content is fetched by
# install scripts). This script downloads the two model tarballs the
# plugin's Model dropdown offers, into third_party/models/.
#
# The plugin loads the .tar.gz paths directly at runtime (the same
# pattern as the deep-filter binary's -m flag): no extraction, no
# weights in the repo. The install/build scripts copy these tarballs
# into the plugin bundles' models/ directories, which is where the
# plugin looks for them (after $NOISE_ORDNANCE_MODEL_DIR).
#
# Sizes below are the sizes recorded when this branch was created
# (GitHub API, Oct 2026). A mismatch fails the download with a warning
# -- it usually means the upstream file changed, in which case the
# per-model latencies in dsp/DenoiserEngine.cpp must be re-measured
# (see tests/latency-by-model.sh) before trusting the build.
#
# Usage: bash third_party/download-models.sh
# Run from the project root (or anywhere: it resolves its own dir).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$ROOT/third_party/models"
mkdir -p "$OUT"

# name, expected size in bytes
# NOTE: DeepFilterNet2 is intentionally NOT listed here — DF2 crashes
# the host at load under libDF/tract (see BRANCH-NOTES.md), so there
# is no reason to download its model. (A leftover bare-size entry for
# it once made this script 404 on a nonsense filename and abort
# before fetching the real models; do not re-add it.)
MODELS=(
  "DeepFilterNet3_onnx.tar.gz 7983136"
  "DeepFilterNet3_ll_onnx.tar.gz 36359660"
)
BASE="https://github.com/Rikorose/DeepFilterNet/raw/main/models"

echo "=== Noise Ordnance: downloading DeepFilterNet models to $OUT ==="
for entry in "${MODELS[@]}"; do
  name="${entry%% *}"
  expected="${entry##* }"
  dest="$OUT/$name"
  if [[ -f "$dest" ]]; then
    actual="$(stat -c%s "$dest")"
    if [[ "$actual" == "$expected" ]]; then
      echo "--- $name: already present, size OK ($actual bytes), skipping ---"
      continue
    fi
    echo "--- $name: present but size $actual != expected $expected, re-downloading ---"
  fi
  echo "--- downloading $name ---"
  curl -L --fail -o "$dest" "$BASE/$name"
  actual="$(stat -c%s "$dest")"
  if [[ "$actual" != "$expected" ]]; then
    echo "WARNING: $name downloaded size $actual != expected $expected."
    echo "  Upstream may have changed the file. Re-measure per-model"
    echo "  latencies (tests/latency-by-model.sh) before trusting this build."
  else
    echo "    OK: $name ($actual bytes)"
  fi
done

echo ""
echo "=== Models in $OUT ==="
ls -la "$OUT"
