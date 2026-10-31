# DeepFilterNet engine branch — notes

Branch `deepfilter-engine`, off main. Test branch only: it converts the
Noise Ordnance engine from RNNoise to DeepFilterNet. No PR, no merge
until Jay has played with the knobs and decided.

## What changed vs main

- **Model core:** RNNoise is gone. The engine now calls libDF (the Rust
  core of Rikorose/DeepFilterNet, pinned at main commit `d375b2d`,
  Oct 2024) through its plain C API (`libDF/src/capi.rs`), declared
  for C++ in `dsp/df.h`. No Rust code lives in this repo; the plugin
  links libDF as a static library.
  - **Pin history:** the branch briefly tried v0.5.6 (tract 0.19.16)
    hoping it would load DeepFilterNet2, but DF2 fails to load under
    both versions (see the DF2 section above), so there was no reason
    to carry v0.5.6's older tree: main builds on current stable Rust,
    declares the staticlib crate-type already, and gives identical
    DF3/DF3_ll behavior (same measured latencies under both versions).
    If a future tract fixes DF2's lowering, re-pinning is a one-line
    change plus a latency re-measurement.
- **Kept as-is:** the DPF plugin shell (LV2 + VST3), the hum-filter
  notch bank, Input/Output Trim, Reduction wet/dry mix, Bypass, Delta
  Monitor (dry-minus-mixed is model-agnostic), and the Speex
  resample-to-48k layer (DeepFilterNet is 48 kHz-only, like RNNoise
  was). The test-harness structure is kept where it still applies.
- **Dropped:** the RNNoise-specific params — Smoothing, Floor
  Low/Mid/High, VAD Adapt — and the RNNoise clone/patch steps in
  `third_party/install-third-party.sh` (including the
  `third_party/rnnoise-noise-ordnance.patch` file, deleted).
- **Model files are not in git.** They are downloaded by
  `third_party/download-models.sh` into `third_party/models/` (see
  "How to build" below) and copied into each bundle's `models/`
  directory by the build scripts.

## New parameter list (10)

| # | Knob | Range | Default |
|---|------|-------|---------|
| 0 | Model | DeepFilterNet3 / DeepFilterNet3 Low Latency | DeepFilterNet3 |
| 1 | Reduction | 0–100 % | 100 % |
| 2 | Input Trim | −12 to +12 dB | 0 dB |
| 3 | Output Trim | −12 to +12 dB | 0 dB |
| 4 | Bypass | on/off | off |
| 5 | Hum Filter | on/off | off |
| 6 | Mains Freq | 50 / 60 Hz | 60 Hz |
| 7 | Post Filter Beta | 0–0.05 | 0.02 (0 = post-filter off) |
| 8 | Attenuation Limit | 0–100 dB | 100 dB (100 = no limit; lower = less suppression) |
| 9 | Delta Monitor | on/off | off |

The Model param is a DPF enumeration parameter, so hosts render a
dropdown (LV2 scale points), not a plain integer stepper. The
parameter contract test (`tests/param-contract.py`) pins the scale
points in the generated TTL.

Model switching reloads synchronously on param change (load time was
explicitly not a concern for this branch). On switch, the engine
updates its reported latency via `setLatency`, so the host
re-compensates — but see "Known limitations" below.

## DeepFilterNet2 is NOT in this branch (read this first)

The original plan offered three models with DeepFilterNet2 as the
default. DF2 cannot ship: **both DF2 models (full and `_ll`) abort the
host process at load time.** `df_create` panics inside tract's reshape
lowering (`todo!()`, "not yet implemented",
tract-hir `ops/array/reshape.rs`, reached via `DfTract::new` →
`into_typed`) — this is model-graph loading, before any audio, and it
is fatal (Rust panic-abort across the FFI boundary; no graceful
fallback is possible from C++). Verified in three independent ways:

- This engine's `df_create` path, under libDF main (tract 0.21.4) and
  under v0.5.6 (tract 0.19.16) — same panic, different tract file
  (`change_axes.rs` on main, `reshape.rs` on 0.5.6).
- The repo's own `deep-filter` binary built from v0.5.6, with
  `-m DeepFilterNet2_onnx.tar.gz` — same panic.
- The DF2 low-latency variant (`DeepFilterNet2_onnx_ll.tar.gz`) —
  same panic.

Only the DF3 family loads via libDF/tract. The DF2 model files are
identical across libDF revisions (same git blobs), so this is a
tract-lowering limitation on DF2's graph, not a corrupt download.
Until tract can lower DF2's Reshape (or libDF gains a non-tract
backend), DF2 stays out of the dropdown. The branch default is
therefore DeepFilterNet3.

A note on the "DF2 baseline": the Oct 30 deep-filter listening run
that produced the combo numbers (peaks −0.2 dB, pauses to
near-silence) was almost certainly DF3, not DF2. The release binary's
embedded default model is DeepFilterNet3 (`DfParams::default()` loads
the embedded DF3 tarball), and no `-m` flag was recorded for that run
— a `-m DF2` run would have crashed, not produced audio. So DF3 as
the branch default matches the actual measured baseline, not just the
label that was attached to it. Confirm the exact command if it matters;
the branch numbers below are DF3's either way.

## Per-model measured latencies

Measured by speech cross-correlation (`tests/check-latency.py`): the
clean drill speech is run through `tests/denoise_wav` at 100% wet, and
a mid-file segment (5–15 s) of input vs output is correlated; the
correlation peak lag is the delay. An impulse probe does not work for
DF (the model gates near-silent frames — a lone impulse gives no
output under 0.5.6 and only a marginal 1.6e-4 peak under main), and a
noise burst does not work either (DF suppresses stationary noise below
any useful threshold). Speech is what DF passes, so speech measures it.
The unit test (`tests/unit/test-latency.cpp`) repeats the same
correlation in-process and must agree with the table to the sample
(it did: diff +0 for both models, peak correlations 0.996 and 1.000).
The numbers below are what the engine reports via `latencySamples()`
— the table in `dsp/DenoiserEngine.cpp` (`kModelLatency48k`),
rate-scaled for other host rates. Each total = wetStart 4096 + chain
lag (model STFT + lookahead + resamplers).

| Model | Latency @ 48 kHz (samples) | Latency @ 48 kHz (ms) |
|-------|---------------------------|----------------------|
| DeepFilterNet3 | 5616 | 117 |
| DeepFilterNet3 Low Latency | 4656 | 97 |

The same two numbers came out of the impulse test under libDF main
(53616 − 48000 = 5616; 52656 − 48000 = 4656) and out of correlation
under 0.5.6 — the model delay is identical across libDF versions, so
the table survives a re-pin. Still, re-measure if the model files
change: a new upstream tarball can change a model's internal delay,
and a stale table number makes the host compensate by the wrong
amount. After editing the table, rebuild and re-run
`tests/check-latency.py` plus the latency unit test — both must pass.

DeepFilterNet2 has no row: it does not load (see above), so it has no
meaningful latency here.

## How to build

Prerequisites: C++17 toolchain, `make`, `pkg-config`, `git`, `python3`,
SpeexDSP (`libspeexdsp-dev` on Debian/Ubuntu), and a Rust toolchain
(rustup: https://rustup.rs — `cargo` must be on PATH; first libDF
build downloads crates from crates.io and takes a while, and the cargo
target dir is large — allow ~2 GB).

```bash
bash third_party/install-third-party.sh   # DPF + DeepFilterNet (pinned main d375b2d) + libDF build + model download
make                                      # LV2 + VST3 bundles in third_party/bin/
bash build-test-install.sh                # full build + regression tests + install prompt
bash validate-build.sh                    # full build + full test set, no install (CI-style check)
bash debug.sh                             # quick debug build, installs to ~/.lv2 + ~/.vst3, runs NO tests
```

Runtime model resolution, in order: `$NOISE_ORDNANCE_MODEL_DIR` if set
and non-empty; else `<bundle>/models/` (the bundle path comes from
DPF's `Plugin::getBundlePath()` — set by the LV2 and VST3 wrappers at
instantiate time; the VST3 layout uses the same `<bundle>/models/`
convention as a best-effort). A bundle without models runs dry
passthrough — it never crashes on a missing file, but it also never
denoises. The smoke test fails a bundle missing any tarball.

## How to run tests

```bash
bash tests/run_drills.sh            # audio drills (gated hiss + hum, report-only combo + quiet)
bash tests/run-unit-tests.sh        # engine unit tests (silence, impulse, samplerate, bypass, block invariance)
bash tests/lv2-smoke-test.sh        # LV2 bundle structure + models + discoverability
python3 tests/param-contract.py     # 10 params vs README table + Model scale points (TTL or source)
bash tests/test-harness-settings.sh # denoise_wav honors model/pfbeta/atten args
bash tests/check-pumping.sh         # pumping check (report-only by design)
bash tests/latency-by-model.sh      # per-model impulse latencies (see above)
```

`tests/denoise_wav` is the offline harness: `denoise_wav in.wav out.wav
[blocksize] [reduction01] [hum] [model] [pfbeta] [atten]`. Model files
come from `$NOISE_ORDNANCE_MODEL_DIR`, else `./third_party/models`. A
missing model is a hard error (exit 1), never a silent dry run.

## Validation results (this branch, on the build VM)

`bash validate-build.sh` — ALL GREEN, Oct 31, 2026, libDF main
(`d375b2d`, stable Rust), SpeexDSP 1.2.1. Plugin builds LV2 + VST3,
TTL generated with the Model enum scale points.

Drills (`tests/run_drills.sh`, DF3 default settings, blocks 128 and
1024 — identical numbers at both blocks, which is itself a
block-invariance check on the audio path):

| Drill | Floor reduction | Speech peaks | Gate |
|-------|----------------|--------------|------|
| hiss | +53.4 dB (−45.3 → −98.7) | −0.2 dB | ≥20 dB / ±1 dB: PASS |
| hum (notch module on, 60 Hz) | +76.5 dB (−41.1 → −117.6) | −0.8 dB | ≥40 dB / ±1 dB: PASS |
| combo (report-only) | +58.7 dB (−39.7 → −98.4) | −0.8 dB | — |
| quiet (report-only) | floor to meter bottom (−60.4 → −240.0; effectively digital silence, do not quote +179.6 dB as a real figure) | −43.4 dB (quiet room tone is crushed too — expected at 100% on a speech model) | — |

Unit tests (`tests/run-unit-tests.sh`): 5/5 — silence, bypass
(192000/192000 bit-identical at delay 5616), latency (reported 5616,
correlation peak 5616, diff +0, correlation 1.000), samplerate (44.1 k
latency 5160 and 96 k latency 11232, both exactly rate-scaled from
5616), block-invariance (bit-identical at 128/256/512/1024/2048 @48 k,
128 vs 1024 @44.1/96 k, defaults + tuned).

Latency script (`tests/check-latency.py`): both models exact —
DF3 table 5616 = measured 5616, DF3_ll table 4656 = measured 4656
(tolerance ±48).

LV2 smoke test: bundle well-formed, both model tarballs present in
`models/` (lv2ls not installed on this VM, so the discoverability
check was skipped, not failed).

Parameter contract: all 10 parameters match, TTL mode and source
mode, including the Model enum scale-point pin.

Harness settings: default vs tuned (pfbeta 0.05, atten 50) differ and
the tuned run still passes the hiss gates; model 1 vs default differ
(the model arg is honored).

Pumping check (report-only, ungated by design): 2293 windows over the
processed quiet drill; mean −211.06 dB; overall std 54.10 dB, dominated
by 1782 fully-silent windows; non-silent windows (511) mean −110.14 dB,
std 5.07 dB, range 26.02 dB. Honest read: DF crushes quiet to silence
most of the time but lets occasional bursts through, so its quiet
output is burstier than the RNNoise engine's was (non-silent std 0.80 dB
in the RNNoise era). Whether that reads as "breathing" needs a listen;
the metric stays ungated until then.

Failures found and fixed during this validation (all in branch test
code, none in the engine): a stale `unit-test-impulse` binary kept
running after the test file was deleted (build-unit-tests.sh now
cleans stale binaries first); run_drills.sh invented measure.py flags
that do not exist (now uses `--min-floor-reduction-db` /
`--max-peak-delta-db`, and gated runs no longer pass `--check-only`,
which had silently disabled gating); the param-contract scale-point
check matched `rdfs:label "` with one space where the TTL writes two
(now matches the quoted label); denoise_wav moved to tests/build/ so
the pumping step can find it (it previously skipped).

## Known limitations

- **Sync model reload dropout.** Switching models reloads synchronously
  on the audio thread: the pipeline clears, so there is a brief dry
  moment at the switch, and the load itself blocks audio for the load
  time. Fine for testing; a shipping version would want async loading
  with crossfade.
- **Latency change on switch.** Different models have different
  latencies (see table above). The plugin reports the new latency via
  `setLatency`, but whether a given host re-compensates mid-session
  (vs only at instantiate) is host-dependent — Ardour re-reads it on
  port changes in some paths and not others. If a switch sounds
  misaligned, remove and re-add the instance.
- **CPU unmeasured on this VM vs Jay's machine.** No real-time CPU
  measurement was taken on this branch; judge real-time use in a host
  (Ardour load, or the LADSPA plugin) on your own machine before
  deciding the DF engine is viable for tracking-adjacent work.
- **Embedded default-model bloat.** libDF's `capi` feature embeds a
  default DFN3 model (~8 MB) that this plugin never uses (models load
  from files). The static library carries it anyway. A future cleanup
  could build libDF without the `default-model` feature — that needs a
  small libDF patch, since `capi` currently enables it.
- **Corrupt model file risk.** libDF's `df_create` panics (aborts) on
  an unreadable model path. The engine stat-checks the file before
  calling it, so a *missing* file is safe — but a *corrupt* file (bad
  tarball, truncated download) would still abort the host. The
  download script's size checks are the mitigation; verify a fresh
  download's sizes before trusting a build.
- **VST3 bundle-path layout is best-effort.** LV2 bundle resolution is
  verified via DPF's LV2 wrapper; VST3 uses the same
  `<bundle>/models/` convention but was not verified against a VST3
  host's bundle layout on this branch.
- **Wet-start invariant.** The engine gates wet output on a fixed
  absolute position (`wetStart_`, 4096 samples at 48 kHz, rate-scaled)
  so output is block-size invariant regardless of model lookahead +
  resampler delays. If block invariance ever breaks (e.g., a new model
  with much larger lookahead), bump `kWetStart48k` in
  `dsp/DenoiserEngine.cpp` — but note the wet-start prefix is also the
  reason the first ~85 ms of a stream is dry, by design.
- **Plugin without models runs dry.** By design (never crash on a
  missing file), but it means a silent "it loads but does nothing"
  failure mode exists. The smoke test and the unit-test/drill
  model checks exist to catch it before it reaches a host.
