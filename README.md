# Noise Ordnance

Built by **Jay Peterman**.

A neural-network noise reduction plugin for Linux (LV2, with VST3 from the same build). DeepFilterNet kills hiss and broadband noise; a traditional notch cascade handles mains hum. Built for post-production (podcast dialogue, stream VODs, recorded tracks) where you want the noise gone and the voice untouched.

- 📖 **Documentation:** https://plugins.johnpeterman.me/noise-ordnance
- 💻 **Source:** https://github.com/johnmp4/noise-ordnance
- 📦 **Releases:** https://github.com/johnmp4/noise-ordnance/releases
- 🐛 **Issues:** https://github.com/johnmp4/noise-ordnance/issues

**What it is not:** a live instrument effect. The model works in 10 ms frames at 48 kHz, and the engine's wet-start prefix plus STFT + lookahead bring the measured total latency to 117 ms (DeepFilterNet3) or 97 ms (DeepFilterNet3 Low Latency) at 48 kHz — see "Measured performance" below and BRANCH-NOTES.md for the per-model numbers and how they were measured. Latency is reported to the host for delay compensation — don't monitor through it.

## The chain

Dual-mono: stereo is two independent engines, one per channel. Each channel runs, in order:

1. **Hum notch filter** — cascaded biquads at 50 or 60 Hz plus the first four harmonics. Old-school DSP, runs before everything else. Toggleable.
2. **DeepFilterNet denoiser** — the neural net (deep filtering: it predicts complex filter coefficients, not just per-band gains), running at its native 48 kHz with resampling on both sides. This is the hiss killer. Two models, switchable from the Model dropdown: DeepFilterNet3 (default) and DeepFilterNet3 Low Latency (no lookahead, for the lowest-latency use). DeepFilterNet2 is not offered: both DF2 models abort the host process at load time under libDF's tract backend (see BRANCH-NOTES.md).
3. **Post-filter** — DeepFilterNet's perceptual cleanup stage, after the model. The Post Filter Beta knob (0–0.05, default 0.02; 0 = off) sets its strength.
4. **Attenuation limit** — a global cap on how hard the model can cut, in dB (0–100, default 100 = no limit). Lowering the cap leaves more of the original in the output: it is the DF-native equivalent of the old attenuation floors, but one global number instead of per-band values. This is the knob that keeps the "metallic" edge out.
5. **Wet/dry mix** — the Reduction knob, with smoothed transitions so automation doesn't zip.

## Parameters (10)

| # | Knob | Range | Default | Notes |
|---|------|-------|---------|-------|
| 0 | Model | DeepFilterNet3 / DeepFilterNet3 Low Latency | DeepFilterNet3 | Dropdown (LV2 enumeration); switching reloads the model and updates reported latency |
| 1 | Reduction | 0–100 % | 100 % | Wet/dry mix |
| 2 | Input Trim | −12 to +12 dB | 0 dB | Hit the model at a healthy level |
| 3 | Output Trim | −12 to +12 dB | 0 dB | Makeup after the fact |
| 4 | Bypass | on/off | off | True bypass (delay-compensated dry) |
| 5 | Hum Filter | on/off | off | The notch cascade |
| 6 | Mains Freq | 50 / 60 Hz | 60 Hz | Match your wall power |
| 7 | Post Filter Beta | 0–0.05 | 0.02 | 0 = post-filter off |
| 8 | Attenuation Limit | 0–100 dB | 100 dB | 100 = no limit; lower = less suppression |
| 9 | Delta Monitor | on/off | off | Outputs dry-minus-wet: hear what's being removed |

### Tuning with the delta monitor

1. Enable Delta Monitor and set the attenuation limit to 100 (no limit).
2. Listen to what's being removed — heavy voice ghosting means the model is cutting too hard.
3. Lower the attenuation limit until the voice in the delta is faint. A little ghosting is normal: the model always takes some voice along with the noise, so don't chase a delta with no voice in it at all. Note the direction: with the old per-band floors you *raised* the floor to ease off; with the attenuation cap you *lower* the cap.
4. Disable Delta Monitor. That's your sweet spot.

## Measured performance

From the repo's drill suite, real speech + noise recordings, not synth tones (DeepFilterNet is a speech model, so a sine wave measures nothing meaningful):

- DeepFilterNet3 on the hiss+hum combo drill in this branch's suite (default settings, report-only): floor +58.7 dB, speech peaks −0.8 dB — see BRANCH-NOTES.md for the full drill table from this branch's validation run. (Jay's Oct 30 binary run on his own combo file measured peaks −0.2 dB; different file, different harness — both are DF3 numbers.)
- Engine through the hiss drill: floor reduction and peak figures are in BRANCH-NOTES.md (this branch's validation run; the suite gates are ≥20 dB floor reduction, peaks within ±1 dB, at block sizes 128 and 1024).
- Engine through the hum drill (hum module on): ≥40 dB floor reduction gate — set so a broken hum module fails.
- Two-stage practical example (Noise Ordnance + Noise Repellent LV2, RNNoise era): Ordnance run gentle, then Repellent learns a pause. On the hiss+hum combo drill: **+27.5 dB** floor reduction, speech peaks −2.9 dB. Realistic numbers for a full chain tuned by ear, not a maximum-reduction figure. (Pre-DF numbers; re-tune with the DF engine before quoting for it.)

## Building

Prerequisites: a C++17 toolchain, `make`, `pkg-config`, `git`, `python3` (for the test suite), SpeexDSP (`libspeexdsp-dev` on Debian/Ubuntu — resampling only), and a Rust toolchain for the libDF build (install via rustup: https://rustup.rs — `cargo` must be on PATH).

```bash
bash third_party/install-third-party.sh   # clones DPF + DeepFilterNet (pinned), builds libDF, downloads models
make                                      # LV2 + VST3 bundles land in third_party/bin/
bash build-test-install.sh                # full build + regression tests + install prompt
```

The install script pins DeepFilterNet to main commit `d375b2d` and resets before building, so the model side of the build is deterministic (DPF currently tracks master). DeepFilterNet2 is not loadable under this pin or under v0.5.6 (tract cannot lower DF2's graph — see BRANCH-NOTES.md), which is why the dropdown offers only the DF3 models. Model files are downloaded, not committed: the three tarballs land in `third_party/models/` and are copied into each bundle's `models/` directory by the build scripts. The plugin finds them at runtime in this order: `$NOISE_ORDNANCE_MODEL_DIR` if set, else `<bundle>/models/`. A bundle without models runs dry passthrough — it never crashes on a missing file, but it also never denoises, which is why the smoke test checks for the tarballs.

**Debug builds:** `bash debug.sh` (or `make DEBUG=true`) — full logging to `/tmp/no-debug.log` through `DenoiserEngine::debugLog()`, the single place the log path lives. Build debug to test, rebuild normally for release; the logging compiles out. debug.sh runs no tests.

Tested in Ardour. When rebuilding while an old instance sits on a track: delete the instance first (bypass isn't enough — Ardour holds the old binary in memory), rebuild, reinstall, re-add.

## Testing

`tests/run_drills.sh` runs real recordings through the actual engine (`tests/denoise_wav`, an offline harness around `dsp/DenoiserEngine.cpp` — no DPF, no host, no audio device) and scores them with `tests/measure.py`.

Gated (must pass, at block sizes 128 and 1024):

- Hiss drill: ≥20 dB floor reduction, speech peaks within ±1 dB.
- Hum drill (hum module on): ≥40 dB floor reduction — set so a broken hum module fails the gate.

Report-only: the hiss+hum combo drill, and the no-speech quiet guard (the floor must not rise; listen to the processed quiet file — it should sound steady and quiet, not swelling/breathing or chirpy).

Only the clean speech file ships in the repo; the noisy variants are derived locally on the first run (`tests/derive-drills.py`, deterministic, seed 42). Replace the clean file with your own speech, delete the derived files, and re-run to test against your own audio. See `tests/drills/README.md` for details.

Do not test the model with synthetic tones: DeepFilterNet is a speech model, so a sine measures nothing meaningful. Tones belong in unit tests of the hum filter alone, with the model out of the path.

## Roadmap

- **GUI / commercial version:** when someone asks. The open LV2 is the product for now.

## Contributing

Bug reports and pull requests are welcome — please open an issue first for anything bigger than a typo so the approach can be agreed on. Run the drill suite (`tests/run_drills.sh`) before submitting DSP changes.

## License

MIT — see [LICENSE](LICENSE).

Copyright (c) 2026 John Peterman.

Third-party components keep their own licenses, and their notices must stay intact in any distribution:

- **DeepFilterNet** — MIT / Apache-2.0 (© Rikorose and contributors)
- **DPF (DISTRHO Plugin Framework)** — MIT
