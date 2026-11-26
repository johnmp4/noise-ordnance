#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# build-test-install.sh: the full local pipeline -- validate the build
# (validate-build.sh: build all targets + full test set), then install
# the built bundles (install.sh). A thin wrapper kept for convenience;
# whether to remove it entirely is still an open question.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

bash validate-build.sh
bash install.sh

echo "=== Noise Ordnance: done ==="
