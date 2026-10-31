#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# check-latency.py: verify the engine's reported per-model latency
# against the TRUE wet-path delay, measured by cross-correlation.
#
# Why correlation, not an impulse: DeepFilterNet gates near-silent
# frames to exact zero (its low-SNR handling), so a lone impulse in
# silence produces no measurable output at all under libDF 0.5.6 -- the
# impulse probe that worked for RNNoise is meaningless for DF. Speech
# is what DF passes, so speech is what measures it: run the clean drill
# speech through tests/build/denoise_wav at 100% wet, correlate a mid-file
# speech segment of input vs output, and the lag of the correlation
# peak is the true delay in samples. Mid-file only: the stream's first
# ~85 ms is dry (the wet-start prefix) and the first ~10 ms is the mix
# ramp, so both are excluded.
#
# The script reads the expected values from kModelLatency48k in
# dsp/DenoiserEngine.cpp (the table the engine reports via
# latencySamples()) and FAILS if any model's measured delay differs by
# more than 48 samples (1 ms at 48 kHz). Re-measure with this script
# whenever the model files change; the table must match the files on
# disk or host delay compensation is wrong by the difference.
#
# Usage: python3 tests/check-latency.py [--models 0,1,2]
# Requires: tests/build/denoise_wav built (tests/run_drills.sh builds it),
# the clean drill file, and the model tarballs
# ($NOISE_ORDNANCE_MODEL_DIR or third_party/models).
import argparse
import os
import re
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODELS = ["DeepFilterNet3", "DeepFilterNet3 Low Latency"]
# Mid-file speech segment (samples): avoids the wet-start prefix and mix ramp.
SEG_START = 5 * 48000
SEG_LEN = 10 * 48000
MAX_LAG = 12000
TOLERANCE = 48


def read_wav(path):
    with wave.open(str(path)) as w:
        n = w.getnframes()
        raw = w.readframes(n)
    return list(struct.unpack("<%dh" % n, raw))


def table_latencies():
    text = (ROOT / "dsp/DenoiserEngine.cpp").read_text()
    m = re.search(r"kModelLatency48k\[.*?\] = \{(.*?)\}", text, re.DOTALL)
    if not m:
        raise SystemExit("could not parse kModelLatency48k from dsp/DenoiserEngine.cpp")
    # Strip // comments first: "DeepFilterNet2" in a comment would
    # otherwise leak its digit into the value list.
    body = re.sub(r"//.*", "", m.group(1))
    return [int(x) for x in re.findall(r"\d+", body)]


def correlate_delay(ref, out):
    """Lag in [0, MAX_LAG] maximizing sum(ref[i] * out[i + lag])."""
    seg = ref[SEG_START:SEG_START + SEG_LEN]
    try:
        import numpy as np

        r = np.asarray(seg, dtype=np.float64)
        o = np.asarray(out, dtype=np.float64)
        best_lag, best_val = 0, -1.0
        # np.correlate on the full output per lag would be O(n*m); instead
        # correlate the segment against each shifted output slice in C.
        for lag in range(MAX_LAG + 1):
            val = float(np.dot(r, o[SEG_START + lag:SEG_START + lag + SEG_LEN]))
            if val > best_val:
                best_val, best_lag = val, lag
        return best_lag
    except ImportError:
        pass
    # Pure-Python fallback: coarse search on decimated signals, then
    # refine at full rate around the coarse peak.
    dec = 10
    r_dec = seg[::dec]
    o_dec = out[SEG_START::dec]
    best_lag, best_val = 0, -1.0
    for lag in range(MAX_LAG // dec + 1):
        val = sum(a * b for a, b in zip(r_dec, o_dec[lag:lag + len(r_dec)]))
        if val > best_val:
            best_val, best_lag = val, lag
    coarse = best_lag * dec
    best_lag, best_val = coarse, -1.0
    for lag in range(max(0, coarse - dec), min(MAX_LAG, coarse + dec) + 1):
        val = sum(a * b for a, b in zip(seg, out[SEG_START + lag:SEG_START + lag + SEG_LEN]))
        if val > best_val:
            best_val, best_lag = val, lag
    return best_lag


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", default="0,1")
    args = ap.parse_args(argv)
    wanted = [int(x) for x in args.models.split(",") if x != ""]

    clean = ROOT / "tests/drills/paint-drying-clean.wav"
    if not clean.is_file():
        print("SKIP: clean drill file missing (%s)" % clean)
        return 0
    denoise = ROOT / "tests/build/denoise_wav"
    if not denoise.is_file():
        print("FAIL: %s not built (run tests/run_drills.sh first)" % denoise)
        return 1
    model_dir = os.environ.get("NOISE_ORDNANCE_MODEL_DIR",
                               str(ROOT / "third_party/models"))
    for f in ("DeepFilterNet3_onnx.tar.gz", "DeepFilterNet3_ll_onnx.tar.gz"):
        if not os.path.isfile(os.path.join(model_dir, f)):
            print("FAIL: model file missing: %s/%s" % (model_dir, f))
            return 1
    env = dict(os.environ, NOISE_ORDNANCE_MODEL_DIR=model_dir)

    table = table_latencies()
    ref = read_wav(clean)
    failures = 0
    outdir = ROOT / "tests/drills/out"
    outdir.mkdir(parents=True, exist_ok=True)
    for idx in wanted:
        dst = outdir / ("latency-model%d.wav" % idx)
        # denoise_wav args: in out block reduction hum model pfbeta atten
        r = subprocess.run([str(denoise), str(clean), str(dst), "128", "1.0",
                            "0", str(idx), "0.02", "100"],
                           env=env, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL model %d (%s): denoise_wav failed: %s"
                  % (idx, MODELS[idx], r.stderr.strip()))
            failures += 1
            continue
        out = read_wav(dst)
        measured = correlate_delay(ref, out)
        expected = table[idx]
        diff = measured - expected
        status = "PASS" if abs(diff) <= TOLERANCE else "FAIL"
        if status == "FAIL":
            failures += 1
        print("%s model %d (%s): table %d, measured %d, diff %+d samples"
              % (status, idx, MODELS[idx], expected, measured, diff))
    if failures:
        print("FAIL check-latency: %d model(s) outside tolerance" % failures)
        return 1
    print("PASS check-latency: all measured delays match the table")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
