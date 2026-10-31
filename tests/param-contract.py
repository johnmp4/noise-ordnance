#!/usr/bin/env python3
# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Parameter contract test: the plugin's 10 parameters must match the README
# table exactly -- names, symbols, ranges, defaults. Two sources of truth,
# same verdict:
#   --ttl PATH     parse the generated LV2 plugin TTL (the artifact hosts
#                  actually read; preferred when a bundle has been built)
#   --source PATH  parse NoiseOrdnance.cpp initParameter (the source the
#                  TTL is generated from; works without a build)
# With neither flag, the TTL bundle is tried first, then the source.
# Why this test exists: the Oct 2026 LV2 generic UI showed 3 unlabeled
# ghost parameters because the param count disagreed across files. A set
# comparison (exactly these 10 symbols, no extras, no missing) catches
# that whole bug class, and the value comparison catches drifted ranges
# or defaults.
#
# Model dropdown pin (TTL mode only): the "model" port must be a DPF
# enumeration (lv2:enumeration + scale points for both models), not
# a plain integer stepper -- that is what makes hosts render a dropdown.
# The check runs on the raw TTL text whenever TTL mode is used.
import re
import sys
from pathlib import Path

# symbol -> (name, minimum, maximum, default). From the README table,
# verified against NoiseOrdnance.cpp initParameter.
EXPECTED = {
    "model": ("Model", 0, 1, 0),
    "reduction": ("Reduction", 0, 100, 100),
    "input_trim": ("Input Trim", -12, 12, 0),
    "output_trim": ("Output Trim", -12, 12, 0),
    "bypass": ("Bypass", 0, 1, 0),
    "hum_enable": ("Hum Filter", 0, 1, 0),
    "hum_freq": ("Mains Freq", 50, 60, 60),
    "post_filter_beta": ("Post Filter Beta", 0, 0.05, 0.02),
    "atten_lim": ("Attenuation Limit", 0, 100, 100),
    "delta_monitor": ("Delta Monitor", 0, 1, 0),
}

# Model scale points the TTL must carry for the dropdown (value -> label).
MODEL_SCALE_POINTS = {
    "DeepFilterNet3",
    "DeepFilterNet3 Low Latency",
}


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
    # Model dropdown pin: in TTL mode the model port must carry
    # lv2:enumeration and both scale-point labels. (Source mode has
    # no TTL text, so it cannot check this; the TTL run in validate-build
    # is the pin.)
    if ttl_text is not None:
        # Model dropdown pin: the model port must carry lv2:enumeration
        # and both scale-point labels. The region from the model port's
        # symbol to the next port's symbol is exactly the model port
        # (DPF writes ports as one Turtle list, so a bracket regex
        # cannot delimit them: the scalePoint entries are nested
        # brackets and later ports omit the `lv2:port` prefix).
        m = re.search(r'lv2:symbol "model"', ttl_text)
        if m is None:
            failures.append("model port missing from TTL")
        else:
            nxt = re.search(r'lv2:symbol "', ttl_text[m.end():])
            region = ttl_text[m.start():m.end() + nxt.start()] if nxt else ttl_text[m.start():]
            if "lv2:enumeration" not in region:
                failures.append("model port lacks lv2:enumeration (hosts would show a stepper, not a dropdown)")
            for label in MODEL_SCALE_POINTS:
                if f'"{label}"' not in region:
                    failures.append(f'model port missing scale point "{label}"')
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

    if failures:
        for failure in failures:
            print(f"  MISMATCH {failure}")
        print(f"FAIL param-contract: {len(failures)} mismatch(es)")
        return 1
    print(f"PASS param-contract: all {len(EXPECTED)} parameters match ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
