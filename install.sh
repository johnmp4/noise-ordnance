#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# install.sh: install the already-built plugin bundles to the
# OS-appropriate destinations. No build, no tests -- run a build script
# (validate-build.sh or debug.sh) first.
#
#   Linux:   NoiseOrdnance.lv2 -> ~/.lv2
#            NoiseOrdnance.vst3 -> ~/.vst3
#   Windows: NoiseOrdnance.vst3 -> C:\Program Files\Common Files\VST3
#            (LV2 is not installed on Windows; the script requests
#            administrator approval via UAC when the shell is not elevated)
#
# Usage: bash install.sh  (from the project root)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

LV2_SRC="$ROOT/third_party/bin/NoiseOrdnance.lv2"
VST3_SRC="$ROOT/third_party/bin/NoiseOrdnance.vst3"

install_one() {
  # install_one <bundle-dir> <destination-parent-dir>
  local src="$1" dest_parent="$2"
  local name
  name="$(basename "$src")"
  mkdir -p "$dest_parent"
  rm -rf "$dest_parent/$name"
  cp -a "$src" "$dest_parent/"
  echo "Installed: $dest_parent/$name"
}

case "$(uname -s)" in
  Linux*)
    if [[ ! -d "$LV2_SRC" || ! -d "$VST3_SRC" ]]; then
      echo "FAIL install: built bundles not found in third_party/bin -- run validate-build.sh or debug.sh first"
      exit 1
    fi
    install_one "$LV2_SRC" "$HOME/.lv2"
    install_one "$VST3_SRC" "$HOME/.vst3"
    ;;
  MINGW*|MSYS*)
    if [[ ! -d "$VST3_SRC" ]]; then
      echo "FAIL install: built VST3 bundle not found in third_party/bin -- run validate-build.sh or debug.sh first"
      exit 1
    fi
    # Copying into Program Files needs administrator rights. `net session`
    # succeeds only when elevated, so it is the elevation check. If this
    # shell is not elevated, re-launch this script elevated once (UAC
    # prompt) and report its exit code; the NOISE_ORDNANCE_ELEVATED guard
    # keeps a still-unelevated re-run from looping.
    if ! net session >/dev/null 2>&1; then
      if [[ -z "${NOISE_ORDNANCE_ELEVATED:-}" ]]; then
        echo "Administrator approval is needed to install to C:\Program Files\Common Files\VST3 -- requesting it now."
        NOISE_ORDNANCE_ELEVATED=1 powershell.exe -NoProfile -Command "\$p = Start-Process -FilePath '$(cygpath -w "$(command -v bash)")' -ArgumentList '--login','\"$(cygpath -w "$ROOT/install.sh")\"' -Verb RunAs -Wait -PassThru; exit \$p.ExitCode"
        exit $?
      fi
      echo "FAIL install: still not elevated -- re-run this script from a MinGW64 shell started with 'Run as administrator'."
      exit 1
    fi
    install_one "$VST3_SRC" "/c/Program Files/Common Files/VST3"
    echo "Note: LV2 is not installed on Windows (Linux-only in this project)."
    ;;
  *)
    echo "FAIL install: unsupported OS '$(uname -s)' -- Linux and Windows (MSYS2/MinGW64) only."
    exit 1
    ;;
esac

echo "=== install: done ==="
