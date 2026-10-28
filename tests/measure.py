#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
"""measure.py: score a denoised drill file the honest way.

Compares ORIGINAL vs PROCESSED with the same windowed-RMS method used to
validate the RNNoise model itself (see ai-denoiser-spike-report.md):

  - noise floor  = mean RMS of the quietest 5% of 20 ms windows
  - speech peaks = 99th percentile of window RMS
  - sample count must match exactly (catches dropped/duplicated samples --
    the bug class that starved the wet path in Oct 2026)

A pure sine tone is NOT a valid test signal for this model: RNNoise is built
to preserve tonal content, so a tone measures "no reduction" even when the
model is fine. Use the real drill recordings.

Usage:
  measure.py original.wav processed.wav [--min-floor-reduction-db 20]
             [--max-peak-delta-db 1.0] [--check-only]

Exit 0 = all gated checks pass (or --check-only reporting), 1 = failure.
"""
import argparse
import math
import sys
import wave


def read_mono_f32(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError(f"{path}: only 16-bit WAV supported")
        n = w.getnframes()
        ch = w.getnchannels()
        raw = w.readframes(n)
    import array

    ints = array.array("h", raw)
    if ch > 1:  # mix down
        mono = array.array("d", (0.0,)) * n
        for i in range(n):
            mono[i] = sum(ints[i * ch:(i + 1) * ch]) / ch
    else:
        mono = array.array("d", ints)
    return [s / 32768.0 for s in mono], w.getframerate()


def window_db(samples, rate, win_ms=20):
    wlen = max(1, int(rate * win_ms / 1000))
    out = []
    for i in range(0, len(samples) - wlen + 1, wlen):
        seg = samples[i:i + wlen]
        rms = math.sqrt(sum(s * s for s in seg) / wlen)
        out.append(20 * math.log10(rms + 1e-12))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("processed")
    ap.add_argument("--min-floor-reduction-db", type=float, default=20.0)
    ap.add_argument("--max-peak-delta-db", type=float, default=1.0)
    ap.add_argument("--check-only", action="store_true",
                    help="report numbers, do not gate on thresholds")
    args = ap.parse_args()

    orig, rate_o = read_mono_f32(args.original)
    proc, rate_p = read_mono_f32(args.processed)

    failures = []
    if len(orig) != len(proc):
        failures.append(
            f"sample count mismatch: {len(orig)} vs {len(proc)} "
            "(samples were dropped or duplicated in the wet path)")
    if rate_o != rate_p:
        failures.append(f"sample rate mismatch: {rate_o} vs {rate_p}")

    db_o = window_db(orig, rate_o)
    db_p = window_db(proc, rate_p)
    db_o.sort()
    db_p.sort()
    q = max(1, len(db_o) // 20)
    floor_o = sum(db_o[:q]) / q
    floor_p = sum(db_p[:q]) / q
    peak_o = db_o[int(len(db_o) * 0.99)]
    peak_p = db_p[int(len(db_p) * 0.99)]

    floor_reduction = floor_o - floor_p
    peak_delta = peak_p - peak_o

    print(f"  {'metric':<18}{'original':>12}{'processed':>12}")
    print(f"  {'noise floor':<18}{floor_o:>11.1f} {floor_p:>11.1f}  "
          f"(reduction {floor_reduction:+.1f} dB)")
    print(f"  {'speech peaks':<18}{peak_o:>11.1f} {peak_p:>11.1f}  "
          f"(delta {peak_delta:+.1f} dB)")

    if not args.check_only:
        if floor_reduction < args.min_floor_reduction_db:
            failures.append(
                f"floor reduction {floor_reduction:.1f} dB < "
                f"{args.min_floor_reduction_db:.1f} dB minimum")
        if abs(peak_delta) > args.max_peak_delta_db:
            failures.append(
                f"peak delta {peak_delta:+.1f} dB exceeds "
                f"+/-{args.max_peak_delta_db:.1f} dB (voice damaged?)")

    for f in failures:
        print(f"  FAIL: {f}")
    if failures and not args.check_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
