# Noise Ordnance

Built by **Jay Peterman**.

A neural-network noise reduction plugin for Linux (LV2, with VST3 from the same build). RNNoise kills hiss and broadband noise; a traditional notch cascade handles mains hum. Built for post-production (podcast dialogue, stream VODs, recorded tracks) where you want the noise gone and the voice untouched.

- 📖 **Documentation:** https://plugins.johnpeterman.me/noise-ordnance
- 💻 **Source:** https://github.com/johnmp4/noise-ordnance
- 📦 **Releases:** https://github.com/johnmp4/noise-ordnance/releases
- 🐛 **Issues:** https://github.com/johnmp4/noise-ordnance/issues

**What it is not:** a live instrument effect. The model works in 10 ms frames, and priming plus resampling bring the measured total latency to ~30 ms at 48 kHz. Latency is measured at init and reported to the host for delay compensation — don't monitor through it.

## The chain

Dual-mono: stereo is two independent engines, one per channel. Each channel runs, in order:

1. **Hum notch filter** — cascaded biquads at 50 or 60 Hz plus the first four harmonics. Old-school DSP, runs before everything else. Toggleable.
2. **RNNoise denoiser** — the neural net, running at its native 48 kHz with resampling on both sides. This is the hiss killer.
3. **3-band attenuation floor** — caps how hard any single frequency band can be cut, so the model can't crush bands to digital silence. This is what keeps the "metallic" edge out. Split into three bands so you can open the highs back up without letting the lows breathe.
4. **Temporal smoothing** — two complementary smoothers on the model's per-band gains: the model's built-in one-sided alpha (gains jump up instantly on speech onsets, decay slowly, no pumping) plus a causal EMA that kills watery flicker without adding latency (it only looks backward). One knob drives both.
5. **VAD-adaptive floor** — voice-activity probability modulates the floor: full floor on speech, deeper cut in pauses. Works similarly to a noise gate, but the model itself decides what's voice.
6. **Wet/dry mix** — the Reduction knob, with smoothed transitions so automation doesn't zip.

## Parameters (12)

| # | Knob | Range | Default | Notes |
|---|------|-------|---------|-------|
| 0 | Reduction | 0–100 % | 100 % | Wet/dry mix |
| 1 | Input Trim | −12 to +12 dB | 0 dB | Hit the model at a healthy level |
| 2 | Output Trim | −12 to +12 dB | 0 dB | Makeup after the fact |
| 3 | Bypass | on/off | off | True bypass |
| 4 | Hum Filter | on/off | off | The notch cascade |
| 5 | Mains Freq | 50 / 60 Hz | 60 Hz | Match your wall power |
| 6 | Smoothing | 0–1 | 0.5 | Drives gain smoothing (0 → 0.7) + alpha (0 → 0.8) |
| 7 | Floor Low | −80 to 0 dB | −24 dB | Bands 0–7 |
| 8 | Floor Mid | −80 to 0 dB | −24 dB | Bands 8–15 |
| 9 | Floor High | −80 to 0 dB | −24 dB | Bands 16–21 |
| 10 | VAD Adapt | 0–1 | 0 | 0 = off; 1 = voice gets your floor, pauses get −40 dB |
| 11 | Delta Monitor | on/off | off | Outputs dry-minus-wet: hear what's being removed |

### On the Smoothing knob

Two smoothers, one knob, linear mapping:

- **gain smoothing** (the EMA) = 0.7 × slider — capped at 0.7, where smearing starts getting obvious
- **alpha** (the model's built-in one-sided smoother) = 0.8 × slider — pushed past the stock 0.6 into "you've gone too far" territory at the top end

At 0 both are truly off. Around 0.35–0.45 is the sweet spot from testing. When it sounds smeared, back off. The knob makes it obvious when it's doing too much.

### Tuning with the delta monitor

1. Enable Delta Monitor and set the floors aggressive (−40 dB).
2. Listen to what's being removed — heavy voice ghosting means the floor is too hot.
3. Raise the floors until the voice in the delta is faint. A little ghosting is normal: the model always takes some voice along with the noise, so don't chase a delta with no voice in it at all.
4. Disable Delta Monitor. That's your sweet spot.

## Measured performance

From the repo's drill suite, real speech + noise recordings, not synth tones (RNNoise is designed to preserve tonal content, so a sine wave measures "no reduction" even when everything works):

- Raw model spike: speech peaks within 0.2 dB of the original, and pauses drop to digital silence — the raw model has no attenuation floor, so there is no meaningful floor-reduction number for it. (That silence is exactly what the plugin's floor tempers.)
- Engine through the hiss drill: **+25.6 dB** floor reduction, speech peaks within 0.2 dB, at block sizes 128 and 1024. The current engine is floor-limited by design: the −24 dB floors cap how hard any band can be cut, which is what keeps the metallic edge out.
- Engine through the hiss+hum combo drill (hum module on): **+31.1 dB** floor reduction, speech peaks −0.7 dB (report-only).
- Two-stage practical example (Noise Ordnance + Noise Repellent LV2): Ordnance run gentle — raised floors, backed-off reduction and smoothing, leaving some noise on purpose — then Repellent learns a pause, and VAD Adapt tightens the pauses back up. On the hiss+hum combo drill: **+27.5 dB** floor reduction, speech peaks −2.9 dB. Realistic numbers for a full chain tuned by ear, not a maximum-reduction figure.

## Building

Prerequisites: a C++17 toolchain, `make`, `pkg-config`, `git`, `python3` (for the test suite), and SpeexDSP (`libspeexdsp-dev` on Debian/Ubuntu — resampling only).

```bash
bash third_party/install-third-party.sh   # clones DPF + rnnoise, applies the Noise Ordnance patch
make                                      # LV2 + VST3 bundles land in third_party/bin/
bash build-test-install.sh                # full build + regression tests + install prompt
```

The install script pins rnnoise to xiph commit `70f1d25` and resets before patching, so the RNNoise side of the build is deterministic (DPF currently tracks master). The patch (`third_party/rnnoise-noise-ordnance.patch`) adds four setters to rnnoise: gain smoothing, alpha, 3-band attenuation floor, and VAD-adaptive floor modulation.

**Debug builds:** `bash debug.sh` (or `make DEBUG=true`) — full logging to `/tmp/no-debug.log` through `DenoiserEngine::debugLog()`, the single place the log path lives. Build debug to test, rebuild normally for release; the logging compiles out.

Tested in Ardour. When rebuilding while an old instance sits on a track: delete the instance first (bypass isn't enough — Ardour holds the old binary in memory), rebuild, reinstall, re-add.

## Testing

`tests/run_drills.sh` runs real recordings through the actual engine (`tests/denoise_wav`, an offline harness around `dsp/DenoiserEngine.cpp` — no DPF, no host, no audio device) and scores them with `tests/measure.py`.

Gated (must pass, at block sizes 128 and 1024):

- Hiss drill: ≥20 dB floor reduction, speech peaks within ±1 dB.
- Hum drill (hum module on): ≥40 dB floor reduction — deliberately above what RNNoise alone achieves (~28 dB), so a broken hum module fails the gate.

Report-only: the hiss+hum combo drill, and the no-speech quiet guard (the floor must not rise; listen to the processed quiet file — it should sound steady and quiet, not swelling/breathing or chirpy).

Only the clean speech file ships in the repo; the noisy variants are derived locally on the first run (`tests/derive-drills.py`, deterministic, seed 42). Replace the clean file with your own speech, delete the derived files, and re-run to test against your own audio. See `tests/drills/README.md` for details.

Do not test the model with synthetic tones: RNNoise is designed to preserve tonal content, so a sine measures "no reduction" even when everything works. Tones belong in unit tests of the hum filter alone, with the model out of the path.

## Roadmap

- **GUI / commercial version:** when someone asks. The open LV2 is the product for now.

## Contributing

Bug reports and pull requests are welcome — please open an issue first for anything bigger than a typo so the approach can be agreed on. Run the drill suite (`tests/run_drills.sh`) before submitting DSP changes.

## License

MIT — see [LICENSE](LICENSE).

Copyright (c) 2026 John Peterman.

Third-party components keep their own licenses, and their notices must stay intact in any distribution:

- **RNNoise** — BSD (© Jean-Marc Valin / Mozilla / Xiph.Org Foundation)
- **DPF (DISTRHO Plugin Framework)** — MIT
