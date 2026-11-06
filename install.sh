#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# install.sh: install the already-built plugin bundles to the
# OS-appropriate destinations. No build, no tests -- run a build script
# (validate-build.sh or debug.sh) first.
#
#   Linux:   NoiseOrdnance.lv2 -> ~/.lv2
#            NoiseOrdnance.vst3 -> ~/.vst3
#            NoiseOrdnance.clap -> ~/.clap (models -> ~/.clap/models)
#   Windows: NoiseOrdnance.vst3 -> C:\Program Files\Common Files\VST3
#            NoiseOrdnance.clap -> C:\Program Files\Common Files\CLAP
#            (models -> ...\CLAP\models; LV2 is not installed on
#            Windows; the script requests administrator approval via
#            UAC when the shell is not elevated)
#
# Usage: bash install.sh  (from the project root)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

LV2_SRC="$ROOT/third_party/bin/NoiseOrdnance.lv2"
VST3_SRC="$ROOT/third_party/bin/NoiseOrdnance.vst3"
CLAP_SRC="$ROOT/third_party/bin/NoiseOrdnance.clap"
MODELS_SRC="$ROOT/third_party/models"

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

install_clap() {
  # install_clap <clap-file> <models-source-dir> <destination-dir>
  # CLAP is a single file, not a bundle: the file goes in the CLAP
  # directory and the model tarballs go in models/ beside it, which
  # is where the plugin looks for them (see NoiseOrdnance.cpp).
  local src="$1" models="$2" dest="$3"
  local name
  name="$(basename "$src")"
  if [[ ! -f "$models/DeepFilterNet3_onnx.tar.gz" || ! -f "$models/DeepFilterNet3_ll_onnx.tar.gz" ]]; then
    echo "FAIL install: model tarballs not found in $models -- run third_party/install-third-party.sh first"
    exit 1
  fi
  mkdir -p "$dest" "$dest/models"
  rm -f "$dest/$name" "$dest/models/DeepFilterNet3_onnx.tar.gz" "$dest/models/DeepFilterNet3_ll_onnx.tar.gz"
  cp -a "$src" "$dest/"
  cp -a "$models/DeepFilterNet3_onnx.tar.gz" "$models/DeepFilterNet3_ll_onnx.tar.gz" "$dest/models/"
  echo "Installed: $dest/$name + $dest/models/"
}

case "$(uname -s)" in
  Linux*)
    if [[ ! -d "$LV2_SRC" || ! -d "$VST3_SRC" || ! -f "$CLAP_SRC" ]]; then
      echo "FAIL install: built bundles not found in third_party/bin -- run validate-build.sh or debug.sh first"
      exit 1
    fi
    install_one "$LV2_SRC" "$HOME/.lv2"
    install_one "$VST3_SRC" "$HOME/.vst3"
    install_clap "$CLAP_SRC" "$MODELS_SRC" "$HOME/.clap"
    ;;
  MINGW*|MSYS*)
    if [[ ! -d "$VST3_SRC" || ! -f "$CLAP_SRC" ]]; then
      echo "FAIL install: built VST3 bundle / CLAP file not found in third_party/bin -- run validate-build.sh or debug.sh first"
      exit 1
    fi
    # Copying into Program Files needs administrator rights. `net session`
    # succeeds only when elevated, so it is the elevation check. If this
    # shell is not elevated, re-launch this script elevated once (UAC
    # prompt) and report its exit code; the NOISE_ORDNANCE_ELEVATED guard
    # keeps a still-unelevated re-run from looping.
    if ! net session >/dev/null 2>&1; then
      if [[ -z "${NOISE_ORDNANCE_ELEVATED:-}" ]]; then
        echo "Administrator approval is needed to install to C:\Program Files\Common Files\VST3 and ...\CLAP -- requesting it now."
        NOISE_ORDNANCE_ELEVATED=1 powershell.exe -NoProfile -Command "\$p = Start-Process -FilePath '$(cygpath -w "$(command -v bash)")' -ArgumentList '--login','\"$(cygpath -w "$ROOT/install.sh")\"' -Verb RunAs -Wait -PassThru; exit \$p.ExitCode"
        exit $?
      fi
      echo "FAIL install: still not elevated -- re-run this script from a MinGW64 shell started with 'Run as administrator'."
      exit 1
    fi
    install_one "$VST3_SRC" "/c/Program Files/Common Files/VST3"
    install_clap "$CLAP_SRC" "$MODELS_SRC" "/c/Program Files/Common Files/CLAP"
    echo "Note: LV2 is not installed on Windows (Linux-only in this project)."
    ;;
  *)
    echo "FAIL install: unsupported OS '$(uname -s)' -- Linux and Windows (MSYS2/MinGW64) only."
    exit 1
    ;;
esac

echo "=== install: done ==="
