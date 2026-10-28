#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Pumping check: is the processed quiet file steady, or does its level
# swell and breathe (pumping)? A denoiser that modulates its gain in
# pauses produces audible level motion even when the measured floor looks
# good, so this measures the level motion directly: 20 ms window RMS in
# dB, then the standard deviation, range, and largest single-step change
# across windows. Prints the numbers; with --max-std-db it also gates on
# the standard deviation (see tests/check-pumping.sh for the threshold).
# Usage: pumping-check.py <wav> [--max-std-db DB]
import math
import struct
import sys
import wave


def read_wav(path):
    w = wave.open(path, "rb")
    n = w.getnframes()
    rate = w.getframerate()
    channels = w.getnchannels()
    raw = w.readframes(n)
    w.close()
    ints = struct.unpack(f"<{n * channels}h", raw)
    samples = []
    for i in range(n):
        s = 0.0
        for c in range(channels):
            s += ints[i * channels + c]
        samples.append((s / channels) / 32768.0)
    return rate, samples


def window_db(rate, samples):
    win = max(1, int(0.02 * rate))
    levels = []
    for start in range(0, len(samples) - win + 1, win):
        seg = samples[start:start + win]
        mean_sq = sum(x * x for x in seg) / len(seg)
        levels.append(20.0 * math.log10(math.sqrt(mean_sq) + 1e-12))
    return levels


def stats(levels):
    n = len(levels)
    mean = sum(levels) / n
    var = sum((x - mean) ** 2 for x in levels) / n
    std = math.sqrt(var)
    spread = max(levels) - min(levels)
    max_step = max(
        (abs(levels[i + 1] - levels[i]) for i in range(n - 1)), default=0.0
    )
    return mean, std, spread, max_step


def main(argv):
    if len(argv) < 2:
        print("usage: pumping-check.py <wav> [--max-std-db DB]")
        return 2
    path = argv[1]
    max_std = None
    if "--max-std-db" in argv:
        max_std = float(argv[argv.index("--max-std-db") + 1])
    rate, samples = read_wav(path)
    levels = window_db(rate, samples)
    mean, std, spread, max_step = stats(levels)
    print(f"--- pumping-check: {path}")
    print(f"    windows: {len(levels)} (20 ms each)")
    print(f"    mean window level: {mean:.2f} dB")
    print(f"    std dev of window levels: {std:.2f} dB")
    print(f"    range (max-min): {spread:.2f} dB")
    print(f"    largest single-step change: {max_step:.2f} dB")
    # A fully silent window reads -240 dB (the epsilon floor), which blows
    # up the range/step numbers on its own. Report the non-silent subset
    # alongside so one silent window doesn't masquerade as pumping.
    non_silent = [x for x in levels if x > -200.0]
    if 0 < len(non_silent) < len(levels):
        nmean, nstd, nspread, _ = stats(non_silent)
        print(f"    non-silent windows: {len(non_silent)} "
              f"(mean {nmean:.2f} dB, std {nstd:.2f} dB, range {nspread:.2f} dB); "
              f"{len(levels) - len(non_silent)} fully-silent window(s)")
    if max_std is not None:
        if std <= max_std:
            print(f"PASS pumping-check: std {std:.2f} dB <= {max_std:.2f} dB")
            return 0
        print(f"FAIL pumping-check: std {std:.2f} dB > {max_std:.2f} dB")
        return 1
    print("    (report-only: no gate given)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
