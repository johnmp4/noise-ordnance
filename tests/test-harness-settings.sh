#!/usr/bin/env bash
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Harness settings test: proves tests/build/denoise_wav actually honors its
# DeepFilterNet arguments (model, post-filter beta, attenuation limit),
# instead of silently ignoring them.
#   1. Runs the hiss drill three ways: defaults (model 0, pfbeta 0.02,
#      atten 100), tuned (pfbeta 0.05, atten 50), and model 1
#      (DeepFilterNet3 Low Latency, defaults otherwise).
#   2. The tuned run and the model-1 run must each differ from the
#      default run -- if the args were ignored, the outputs would be
#      identical and this fails.
#   3. The tuned run must still denoise sanely (same gates as the hiss
#      drill: >= 20 dB floor reduction, peaks within 1 dB). The 50 dB
#      attenuation cap leaves headroom above the 20 dB gate, so a sane
#      run passes it; the gate catches a run that stopped denoising.
# Sample counts must match the input exactly in all runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$ROOT/tests/drills"
OUT="$ROOT/tests/drills/out"
mkdir -p "$OUT"

MODEL_DIR="${NOISE_ORDNANCE_MODEL_DIR:-$ROOT/third_party/models}"
for f in DeepFilterNet3_onnx.tar.gz DeepFilterNet3_ll_onnx.tar.gz; do
  if [[ ! -f "$MODEL_DIR/$f" ]]; then
    echo "FAIL: model file missing: $MODEL_DIR/$f"
    echo "  Run: bash third_party/download-models.sh"
    exit 1
  fi
done
export NOISE_ORDNANCE_MODEL_DIR="$MODEL_DIR"

# denoise_wav is built by run_drills.sh; rebuild if missing so this test
# also works standalone.
BIN="$ROOT/tests/build/denoise_wav"
if [[ ! -x "$BIN" ]]; then
  echo "--- denoise_wav missing; building via run_drills.sh prerequisites ---"
  bash "$ROOT/tests/run_drills.sh" >/dev/null 2>&1 || true
fi
if [[ ! -x "$BIN" ]]; then
  echo "FAIL: $BIN not built (run tests/run_drills.sh first)"
  exit 1
fi

SRC="$DIR/paint-drying-hiss.wav"
if [[ ! -f "$SRC" ]]; then
  echo "SKIP: hiss drill file missing ($SRC); run tests/run_drills.sh first."
  exit 0
fi

# denoise_wav args: in out block reduction hum model pfbeta atten.
# Note: pfbeta/atten are only honored when passed positionally, so the
# tuned run passes all earlier args explicitly (hum 0, model 0).
"$BIN" "$SRC" "$OUT/harness-default.wav" 128 1.0 0 0 0.02 100
"$BIN" "$SRC" "$OUT/harness-tuned.wav" 128 1.0 0 0 0.05 50
"$BIN" "$SRC" "$OUT/harness-model1.wav" 128 1.0 0 1 0.02 100

# Counts must match the input exactly.
IN_COUNT="$(python3 -c "import wave; print(wave.open('$SRC').getnframes())")"
for f in harness-default.wav harness-tuned.wav harness-model1.wav; do
  OUT_COUNT="$(python3 -c "import wave; print(wave.open('$OUT/$f').getnframes())")"
  if [[ "$OUT_COUNT" != "$IN_COUNT" ]]; then
    echo "FAIL: $f has $OUT_COUNT samples, input has $IN_COUNT"
    exit 1
  fi
done

# Tuned and model-1 runs must differ from default (args honored).
# Max absolute sample difference in LSB, computed with stdlib wave.
maxdiff() {
  python3 - "$1" "$2" <<'PYEOF'
import sys, wave, struct
def samples(path):
    with wave.open(path) as w:
        n = w.getnframes()
        return struct.unpack("<%dh" % n, w.readframes(n))
a, b = samples(sys.argv[1]), samples(sys.argv[2])
print(max((abs(x - y) for x, y in zip(a, b)), default=0))
PYEOF
}
MAXDIFF_TUNED="$(maxdiff "$OUT/harness-default.wav" "$OUT/harness-tuned.wav")"
MAXDIFF_MODEL1="$(maxdiff "$OUT/harness-default.wav" "$OUT/harness-model1.wav")"
echo "default vs tuned (pfbeta 0.05, atten 50): max diff $MAXDIFF_TUNED LSB"
echo "default vs model 1 (DeepFilterNet3 Low Latency): max diff $MAXDIFF_MODEL1 LSB"
if [[ "$MAXDIFF_TUNED" == "0" ]]; then
  echo "FAIL: tuned run identical to default -- pfbeta/atten args ignored"
  exit 1
fi
if [[ "$MAXDIFF_MODEL1" == "0" ]]; then
  echo "FAIL: model-1 run identical to default -- model arg ignored"
  exit 1
fi

# Tuned run must still pass the hiss gates.
if python3 "$ROOT/tests/measure.py" "$SRC" "$OUT/harness-tuned.wav" --min-floor-reduction-db 20 --max-peak-delta-db 1.0; then
  echo "PASS harness settings: args honored, tuned run still denoises sanely"
else
  echo "FAIL: tuned run failed the hiss gates"
  exit 1
fi
