#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Parameter contract test: the plugin's 10 parameters must match the README
# table exactly -- names, symbols, ranges, defaults, and order. Two sources
# of truth, same verdict:
#   --ttl PATH     parse the generated LV2 plugin TTL (the artifact hosts
#                  actually read; preferred when a bundle has been built)
#   --source PATH  parse NoiseOrdnance.cpp initParameter (the source the
#                  TTL is generated from; works without a build)
# With neither flag, the TTL bundle is tried first, then the source.
# Why this test exists: the LV2 generic UI shows a parameter unlabeled
# when the param count disagrees across files, so a set comparison
# (exactly these 10 symbols, no extras, no missing) catches that whole
# bug class, and the value comparison catches drifted ranges or defaults.
#
# Low Latency switch pin (TTL mode only): the "low_latency" port must be
# a DPF boolean (lv2:toggled), not an enumeration -- that is what makes
# hosts render a labeled toggle. With only two loadable models, a toggle
# (OFF = DeepFilterNet3, ON = DeepFilterNet3 Low Latency) is clearer
# than an enum that may show up as an unlabeled binary switch.
# The check runs on the raw TTL text whenever TTL mode is used.
import re
import sys
from pathlib import Path

# symbol -> (name, minimum, maximum, default). From the README table,
# verified against NoiseOrdnance.cpp initParameter.
EXPECTED = {
    "low_latency": ("Low Latency Model", 0, 1, 0),
    "reduction": ("Reduction", 0, 100, 100),
    "input_trim": ("Input Trim", -12, 12, 0),
    "output_trim": ("Output Trim", -12, 12, 0),
    "bypass": ("Bypass", 0, 1, 0),
    "hum_enable": ("Hum Filter", 0, 1, 0),
    "hum_freq": ("Hum Freq", 50, 60, 60),
    "post_filter_beta": ("Postfilter Strength", 0, 0.05, 0.02),
    "atten_lim": ("Aggressiveness", 0, 100, 100),
    "delta_monitor": ("Delta Monitor", 0, 1, 0),
}

# Display order hosts see: intermixed so related parameters group in flat
# lists (Carla stacks all parameters in port order). Ardour separates
# switches from sliders but preserves each subsequence, so its two-column
# layout pairs the same groups. Both parsers preserve source order (dict
# insertion order), so the found key order must equal this exactly, in
# either mode.
EXPECTED_ORDER = [
    "bypass",
    "input_trim",
    "hum_enable",
    "hum_freq",
    "low_latency",
    "reduction",
    "atten_lim",
    "post_filter_beta",
    "delta_monitor",
    "output_trim",
]


def parse_ttl(text):
    """Return {symbol: (name, min, max, default)} for control ports."""
    found = {}
    for block in re.findall(r"\[(.*?)\]", text, re.DOTALL):
        if "lv2:ControlPort" not in block:
            continue
        sym = re.search(r'lv2:symbol\s+"([^"]+)"', block)
        name = re.search(r'lv2:name\s+"([^"]+)"', block)
        default = re.search(r"lv2:default\s+(-?[\d.]+)", block)
        minimum = re.search(r"lv2:minimum\s+(-?[\d.]+)", block)
        maximum = re.search(r"lv2:maximum\s+(-?[\d.]+)", block)
        if not (sym and name and default and minimum and maximum):
            continue  # e.g. the latency port: control, but no value range
        found[sym.group(1)] = (
            name.group(1),
            float(minimum.group(1)),
            float(maximum.group(1)),
            float(default.group(1)),
        )
    return found


def parse_source(text):
    """Return {symbol: (name, min, max, default)} from initParameter."""
    found = {}
    for case in re.split(r"case \d+:", text)[1:]:
        block = case.split("break;", 1)[0]
        sym = re.search(r'param\.symbol\s*=\s*"([^"]+)"', block)
        name = re.search(r'param\.name\s*=\s*"([^"]+)"', block)
        default = re.search(r"param\.ranges\.def\s*=\s*(-?[\d.]+)f", block)
        minimum = re.search(r"param\.ranges\.min\s*=\s*(-?[\d.]+)f", block)
        maximum = re.search(r"param\.ranges\.max\s*=\s*(-?[\d.]+)f", block)
        if not (sym and name and default and minimum and maximum):
            continue
        found[sym.group(1)] = (
            name.group(1),
            float(minimum.group(1)),
            float(maximum.group(1)),
            float(default.group(1)),
        )
    return found


def main(argv):
    ttl_path = None
    source_path = None
    args = iter(argv[1:])
    for arg in args:
        if arg == "--ttl":
            ttl_path = next(args, None)
        elif arg == "--source":
            source_path = next(args, None)
        else:
            print(f"usage: {argv[0]} [--ttl PATH | --source PATH]")
            return 2

    root = Path(__file__).resolve().parent.parent
    found = None
    mode = ""
    ttl_text = None
    if ttl_path:
        ttl_text = Path(ttl_path).read_text()
        found = parse_ttl(ttl_text)
        mode = f"TTL {ttl_path}"
    elif source_path:
        found = parse_source(Path(source_path).read_text())
        mode = f"source {source_path}"
    else:
        candidates = [
            root / "third_party/bin/NoiseOrdnance.lv2/NoiseOrdnance_dsp.ttl",
            Path.home() / ".lv2/NoiseOrdnance.lv2/NoiseOrdnance_dsp.ttl",
        ]
        for cand in candidates:
            if cand.is_file():
                ttl_text = cand.read_text()
                found = parse_ttl(ttl_text)
                mode = f"TTL {cand}"
                break
        if found is None:
            src = root / "NoiseOrdnance.cpp"
            found = parse_source(src.read_text())
            mode = f"source {src}"

    print(f"--- param-contract: checking against {mode}")
    failures = []
    # Low Latency switch pin: in TTL mode the low_latency port must carry
    # lv2:toggled (DPF boolean) and must NOT carry lv2:enumeration.
    # The region from the low_latency port's symbol to the next port's
    # symbol is exactly the low_latency port (DPF writes ports as one
    # Turtle list, so a bracket regex cannot delimit them: the
    # scalePoint entries are nested brackets and later ports omit the
    # `lv2:port` prefix).
    if ttl_text is not None:
        m = re.search(r'lv2:symbol "low_latency"', ttl_text)
        if m is None:
            failures.append("low_latency port missing from TTL")
        else:
            nxt = re.search(r'lv2:symbol "', ttl_text[m.end():])
            region = ttl_text[m.start():m.end() + nxt.start()] if nxt else ttl_text[m.start():]
            if "lv2:toggled" not in region:
                failures.append("low_latency port lacks lv2:toggled (hosts would not show a toggle)")
            if "lv2:enumeration" in region:
                failures.append("low_latency port still carries lv2:enumeration (stale enum)")
    for symbol, (name, lo, hi, default) in sorted(EXPECTED.items()):
        if symbol not in found:
            failures.append(f"missing parameter: {symbol}")
            continue
        got = found[symbol]
        if got[0] != name or any(abs(a - b) > 1e-6 for a, b in zip(got[1:], (lo, hi, default))):
            failures.append(
                f"{symbol}: expected ({name}, {lo}, {hi}, {default}), got {got}"
            )
    for symbol in sorted(found):
        if symbol not in EXPECTED:
            failures.append(f"unexpected parameter (ghost?): {symbol} -> {found[symbol]}")
    if list(found.keys()) != EXPECTED_ORDER:
        failures.append(
            f"parameter order mismatch: expected {EXPECTED_ORDER}, got {list(found.keys())}"
        )

    if failures:
        for failure in failures:
            print(f"  MISMATCH {failure}")
        print(f"FAIL param-contract: {len(failures)} mismatch(es)")
        return 1
    print(f"PASS param-contract: all {len(EXPECTED)} parameters match ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
