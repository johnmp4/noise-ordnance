#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
"""Derive Noise Ordnance drill files from a clean speech file.

Only the clean file ships in the repo; the noisy variants are generated
locally by this script (run automatically by run_drills.sh on first test,
or by hand). That keeps large files out of the repo and lets contributors
run the tests against their own clean speech: point --clean at your file,
delete any derived files, and re-run.

Levels (chosen against the Paint Drying clean file's measured stats:
peaks -18.4 dB, quietest-5% floor -106 dB, max sample 0.50):
  hiss:  white noise, RMS -45 dB  -> sits ~27 dB below speech peaks
  hum:   60/120/180/240 Hz sines, combined RMS ~-40 dB
  combo: hiss + hum together
  quiet: white noise, RMS -60 dB  -> synthetic room-tone stand-in for the
         no-speech guard (output floor must not rise above it)
Deterministic (seed 42) so re-running reproduces the same files.
Output: 16-bit PCM mono, same sample rate and sample count as clean.

Existing derived files are left alone (delete one to regenerate it),
unless --force is given. Standard library only: no numpy, no scipy.

Usage:
  derive-drills.py [--clean clean.wav] [--out-dir drills/] [--force]
"""
import argparse
import math
import os
import random
import struct
import sys
import wave

SEED = 42
HISS_RMS_DB = -45
HUM_AMPS = {60: 0.0120, 120: 0.0060, 180: 0.0030, 240: 0.0015}
QUIET_RMS_DB = -60


def read_clean(path):
    with wave.open(path, "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        n = w.getnframes()
        if width != 2:
            sys.exit(f"error: {path} must be 16-bit PCM (got {width * 8}-bit)")
        raw = w.readframes(n)
    ints = struct.unpack(f"<{n * channels}h", raw)
    if channels == 1:
        clean = [x / 32768.0 for x in ints]
    else:
        # Mix down to mono, like the denoise_wav harness does.
        clean = [sum(ints[i * channels + c] for c in range(channels))
                 / (channels * 32768.0) for i in range(n)]
    return clean, rate


def write_wav(path, samples, rate):
    ints = [max(-32768, min(32767, int(round(s * 32768)))) for s in samples]
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(ints)}h", *ints))
    peak = max(abs(s) for s in samples)
    print(f"{os.path.basename(path)}: {len(ints)} samples, max {peak:.4f}")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--clean", default=os.path.join(here, "drills",
                                                    "paint-drying-clean.wav"))
    ap.add_argument("--out-dir", default=os.path.join(here, "drills"))
    ap.add_argument("--force", action="store_true",
                    help="regenerate even if the derived file already exists")
    args = ap.parse_args()

    if not os.path.isfile(args.clean):
        sys.exit(f"error: clean file not found: {args.clean}")
    clean, rate = read_clean(args.clean)
    n = len(clean)
    os.makedirs(args.out_dir, exist_ok=True)

    random.seed(SEED)
    hiss_rms = 10 ** (HISS_RMS_DB / 20)
    hiss = [random.gauss(0, hiss_rms) for _ in range(n)]
    hum = [sum(a * math.sin(2 * math.pi * f * (i / rate))
               for f, a in HUM_AMPS.items()) for i in range(n)]
    quiet_rms = 10 ** (QUIET_RMS_DB / 20)
    quiet = [random.gauss(0, quiet_rms) for _ in range(n)]

    targets = {
        "paint-drying-hiss.wav": [c + h for c, h in zip(clean, hiss)],
        "paint-drying-hum.wav": [c + h for c, h in zip(clean, hum)],
        "paint-drying-combo.wav": [c + h + u for c, h, u in zip(clean, hiss, hum)],
        "paint-drying-quiet.wav": quiet,
    }
    for name, samples in targets.items():
        path = os.path.join(args.out_dir, name)
        if os.path.isfile(path) and not args.force:
            print(f"{name}: exists, skipping (delete it to regenerate, "
                  f"or pass --force)")
            continue
        write_wav(path, samples, rate)


if __name__ == "__main__":
    main()
