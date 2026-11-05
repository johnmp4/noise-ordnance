# Noise Ordnance

A neural-network noise reduction plugin for Linux (LV2, with VST3 from the same build). DeepFilterNet kills hiss and broadband noise; a traditional notch cascade handles mains hum. Built for post-production (podcast dialogue, stream VODs, recorded tracks) where you want the noise gone and the voice untouched.

- 📖 **Documentation:** https://plugins.johnpeterman.me/noise-ordnance
- 💻 **Source:** https://github.com/johnmp4/noise-ordnance
- 📦 **Releases:** https://github.com/johnmp4/noise-ordnance/releases
- 🐛 **Issues:** https://github.com/johnmp4/noise-ordnance/issues

**Lookahead is part of the design:** the model uses lookahead to do its best work on tracks you have already recorded, and the plugin reports its latency to the host for delay compensation. Per-model numbers are in "Measured performance" below.

## Install

Today, install is a source build. Packaged releases for Linux and Windows are on the way.

Prerequisites: a C++17 toolchain, `make`, `pkg-config`, `git`, `python3`, and SpeexDSP (`libspeexdsp-dev` on Debian/Ubuntu). The install script sets up a project-local Rust toolchain for you if `cargo` isn't already available.

```bash
bash third_party/install-third-party.sh   # clones DPF + DeepFilterNet (pinned), builds libDF, downloads models
make                                      # LV2 + VST3 bundles land in third_party/bin/
bash build-test-install.sh                # full build + regression tests + install prompt
```

Load the LV2 or VST3 bundle in your host. Model files are downloaded during the install step and copied into each bundle's `models/` directory — if the plugin loads but does nothing, check that directory first: a bundle without models runs dry passthrough by design.

## The chain

Dual-mono: stereo is two independent engines, one per channel. Each channel runs, in order:

1. **Hum notch filter** — cascaded biquads at 50 or 60 Hz plus the first four harmonics. Old-school DSP, runs before everything else. Toggleable.
2. **DeepFilterNet denoiser** — the neural net (deep filtering: it predicts complex filter coefficients, not just per-band gains), running at its native 48 kHz with resampling on both sides. This is the hiss killer. Two models, selected by the Low Latency Model switch: OFF = DeepFilterNet3 (default, post-production), ON = DeepFilterNet3 Low Latency (no lookahead, for the lowest-latency use).
3. **Post-filter** — DeepFilterNet's perceptual cleanup stage, after the model. The Postfilter Strength knob (0–0.05, default 0.02; 0 = off) sets its strength.
4. **Aggressiveness** — a global cap on how hard the model can cut, in dB (0–100, default 100 = no limit). Lowering the cap leaves more of the original in the output. This is the knob that keeps the "metallic" edge out.
5. **Wet/dry mix** — the Reduction knob, with smoothed transitions so automation doesn't zip.

## Parameters (10)

| # | Knob | Range | Default | Notes |
|---|------|-------|---------|-------|
| 0 | Bypass | on/off | off | True bypass (delay-compensated dry, untrimmed — the A/B reference) |
| 1 | Input Trim | −12 to +12 dB | 0 dB | Hit the model at a healthy level |
| 2 | Hum Filter | on/off | off | The notch cascade |
| 3 | Hum Freq | 50 / 60 Hz | 60 Hz | Match your wall power |
| 4 | Low Latency Model | off / on | off | OFF = DeepFilterNet3 (post-production); ON = DeepFilterNet3 Low Latency (no lookahead). Switching reloads the model and updates reported latency; hosts vary in whether they re-compensate mid-session — if a switch sounds misaligned, remove and re-add the instance |
| 5 | Reduction | 0–100 % | 100 % | Wet/dry mix |
| 6 | Aggressiveness | 0–100 dB | 100 dB | 100 = no limit; lower = less suppression |
| 7 | Postfilter Strength | 0–0.05 | 0.02 | 0 = post-filter off |
| 8 | Delta Monitor | on/off | off | Outputs dry-minus-wet: hear what's being removed |
| 9 | Output Trim | −12 to +12 dB | 0 dB | Makeup after the fact (wet signal only; bypass is untrimmed) |

### Tuning with the delta monitor

1. Enable Delta Monitor and set Aggressiveness to 100 (no limit).
2. Listen to what's being removed — heavy voice ghosting means the model is cutting too hard.
3. Lower Aggressiveness until the voice in the delta is faint. A little ghosting is normal: the model always takes some voice along with the noise, so don't chase a delta with no voice in it at all.
4. Disable Delta Monitor. That's your sweet spot.

## Measured performance

From the repo's drill suite, real speech + noise recordings, not synth tones (DeepFilterNet is a speech model, so a sine wave measures nothing meaningful):

### Latency

| Model | Latency @ 48 kHz (samples) | Latency @ 48 kHz (ms) |
|-------|---------------------------|----------------------|
| DeepFilterNet3 | 5616 | 117 |
| DeepFilterNet3 Low Latency | 4656 | 97 |

Measured by speech cross-correlation (`tests/check-latency.py`, confirmed by an in-process unit test that agrees to the sample). Impulse and noise-burst probes do not work for this model: it gates near-silent frames and suppresses stationary noise, so only speech gives a measurable output. Each total is the wet-start prefix (4096 samples) plus chain lag (STFT + lookahead + resamplers); the wet-start prefix also means the first ~85 ms of a stream is dry, by design. Re-measure if the model files change: a new upstream tarball can change a model's internal delay, and a stale table makes the host compensate by the wrong amount.

### Drill results (DeepFilterNet3, default settings)

| Drill | Floor reduction | Speech peaks | Gate |
|-------|----------------|--------------|------|
| hiss | +53.4 dB (−45.3 → −98.7) | −0.2 dB | ≥20 dB / ±1 dB: PASS |
| hum (notch module on, 60 Hz) | +76.5 dB (−41.1 → −117.6) | −0.8 dB | ≥40 dB / ±1 dB: PASS |
| combo (report-only) | +58.7 dB (−39.7 → −98.4) | −0.8 dB | — |
| quiet (report-only) | floor to meter bottom (−60.4 → −240.0; effectively digital silence — do not quote +179.6 dB as a real figure) | −43.4 dB (quiet room tone is crushed too — expected at 100% on a speech model) | — |

## Building

Prerequisites: a C++17 toolchain, `make`, `pkg-config`, `git`, `python3` (for the test suite), SpeexDSP (`libspeexdsp-dev` on Debian/Ubuntu — resampling only), and a Rust toolchain for the libDF build (install via rustup: https://rustup.rs — `cargo` must be on PATH).

```bash
bash third_party/install-third-party.sh   # clones DPF + DeepFilterNet (pinned), builds libDF, downloads models
make                                      # LV2 + VST3 bundles land in third_party/bin/
bash build-test-install.sh                # full build + regression tests + install prompt
```

The install script pins DeepFilterNet to main commit `d375b2d` and resets before building, so the model side of the build is deterministic (DPF currently tracks master). Model files are downloaded, not committed: the three tarballs land in `third_party/models/` and are copied into each bundle's `models/` directory by the build scripts. The plugin finds them at runtime in this order: `$NOISE_ORDNANCE_MODEL_DIR` if set, else `<bundle>/models/`. A bundle without models runs dry passthrough — it never crashes on a missing file, but it also never denoises, which is why the smoke test checks for the tarballs. A missing model file is safe (dry passthrough), but a corrupt one is not: libDF aborts on an unreadable model, so the download script compares each download's size against the expected value and warns on mismatch — don't ignore that warning.

**Debug builds:** `bash debug.sh` (or `make DEBUG=true`) — full logging to `/tmp/no-debug.log` through `DenoiserEngine::debugLog()`, the single place the log path lives. Build debug to test, rebuild normally for release; the logging compiles out. debug.sh runs no tests.

## Testing

`tests/run_drills.sh` runs real recordings through the actual engine (`tests/denoise_wav`, an offline harness around `dsp/DenoiserEngine.cpp` — no DPF, no host, no audio device) and scores them with `tests/measure.py`.

Gated (must pass, at block sizes 128 and 1024):

- Hiss drill: ≥20 dB floor reduction, speech peaks within ±1 dB.
- Hum drill (hum module on): ≥40 dB floor reduction — set so a broken hum module fails the gate.

Report-only: the hiss+hum combo drill, and the no-speech quiet guard (the floor must not rise; listen to the processed quiet file — it should sound steady and quiet, not swelling/breathing or chirpy).

Only the clean speech file ships in the repo; the noisy variants are derived locally on the first run (`tests/derive-drills.py`, deterministic, seed 42). Replace the clean file with your own speech, delete the derived files, and re-run to test against your own audio. See `tests/drills/README.md` for details.

Do not test the model with synthetic tones: DeepFilterNet is a speech model, so a sine measures nothing meaningful. Tones belong in unit tests of the hum filter alone, with the model out of the path.

## Contributing

Bug reports and pull requests are welcome — please open an issue first for anything bigger than a typo so the approach can be agreed on. Before submitting DSP changes, run the drill suite (`tests/run_drills.sh`) and the unit tests (`tests/run-unit-tests.sh`).

## License

MIT — see [LICENSE](LICENSE).

Copyright (c) 2026 John Peterman.

Third-party components keep their own licenses, and their notices must stay intact in any distribution:

- **DeepFilterNet** — MIT / Apache-2.0 (© Rikorose and contributors)
- **DPF (DISTRHO Plugin Framework)** — MIT
