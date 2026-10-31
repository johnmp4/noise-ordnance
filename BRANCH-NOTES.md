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
| 0 | Low Latency Model | off / on | off (DeepFilterNet3) |
| 1 | Reduction | 0–100 % | 100 % |
| 2 | Input Trim | −12 to +12 dB | 0 dB |
| 3 | Output Trim | −12 to +12 dB | 0 dB |
| 4 | Bypass | on/off | off |
| 5 | Hum Filter | on/off | off |
| 6 | Mains Freq | 50 / 60 Hz | 60 Hz |
| 7 | Post Filter Beta | 0–0.05 | 0.02 (0 = post-filter off) |
| 8 | Attenuation Limit | 0–100 dB | 100 dB (100 = no limit; lower = less suppression) |
| 9 | Delta Monitor | on/off | off |

The Low Latency Model param is a DPF boolean, so hosts render a
labeled toggle, not an enum: OFF = DeepFilterNet3 (post-production,
default), ON = DeepFilterNet3 Low Latency (no lookahead). It was an
enumeration originally, but with only two loadable models the enum
showed up in at least one host as an unlabeled binary switch -- the
toggle names what it does. The parameter contract test
(`tests/param-contract.py`) pins `lv2:toggled` on the low_latency port
in the generated TTL. If a third loadable model ever appears, this
switch must become an enumeration again.

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
backend), DF2 stays out. The branch default is
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
python3 tests/param-contract.py     # 10 params vs README table + Low Latency switch toggle pin (TTL or source)
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

Re-validation, Oct 31, 2026 (Low Latency Model switch change): the
Model enumeration became a boolean switch (OFF = DeepFilterNet3,
ON = DeepFilterNet3 Low Latency, default off). `validate-build.sh`
ALL GREEN again on the build VM: drills identical to the run above
(hiss +53.4/−0.2, hum +76.5/−0.8, combo +58.7/−0.8), unit tests 5/5,
latency script both models exact, param contract 10/10 in TTL mode
with the new `lv2:toggled` pin on the `low_latency` port (TTL shows
name "Low Latency Model", default 0, `lv2:portProperty lv2:toggled`,
no enumeration). Engine behavior is unchanged by the switch: only
the plugin parameter changed (plus comments).

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

## Performance work (Oct 31, 2026) -- three improvements, one commit each

Jay's host numbers (Ardour, 48 kHz, mono): buffer 128 -> periodic
xruns on DF3, DF3_ll unusable; buffer 512 -> both sound good, DF3
25-29% CPU, DF3_ll ~50%. Target: 128-sample blocks for
recording/streaming (not live monitoring -- the model's 97/117 ms
total latency rules that out regardless of buffer; see below).

### Leftovers audit: what is RNNoise-era shell vs DF-native?

The heavy CPU is the DF graph itself, not RNNoise leftovers. Audited
Oct 31, 2026:

- DF-native, not tunable without a new model: the 480-sample frame
  (10 ms at 48 kHz, from `df_get_frame_length` -- the ONNX graph's
  STFT hop), the per-frame compute (DF3 8 MB of weights, DF3_ll
  36 MB -- hence LL costing MORE despite the "low latency" name;
  the name means less lookahead, 97 vs 117 ms total, not less CPU),
  and the algorithmic delay (chain lag 1520 DF3 / 560 DF3_ll).
  The C API exposes no frame-size, thread-count, or optimization
  knob (`df_create` takes path/attenuation/log-level only; libDF's
  rayon use is the training dataloader, not inference).
- Shell (ours, inherited from the RNNoise engine architecture, but
  re-justified for DF): `wetStart` 4096 (85 ms) -- a LATENCY cost,
  not a CPU cost; it must exceed the chain lookahead (~2000 worst
  case), and the 2x margin could be trimmed later to cut total
  latency, but that needs a block-invariance + latency re-measure
  and does nothing for CPU. Ring sizes (16384) and scratch (8192)
  are memory, not CPU. Mix smoothing, trims, hum notches (off by
  default), delta monitor: trivial CPU.
- Shell cost that IS real but small: the Speex quality-5 resamplers
  run even at 48 kHz, where they are 1:1 -- and they do NOT pass
  through (verified Oct 31 with a test program: an impulse becomes
  a sinc spread over +-40 samples, 0.94 main lobe, unity gain on
  steady signals). So at 48 kHz they cost two filter passes per
  block plus their delay (inside the measured latency table). A
  bypass-at-48k is a genuine fourth lever, but it changes the
  latency table and needs a re-measure, so it was left out of this
  round -- ask for it if the three below are not enough.
- The structural problem: inference runs synchronously on the audio
  thread (shell design). A 480-sample frame's inference costs more
  than a 128-sample block's 2.7 ms deadline, so 128 xruns even
  though average CPU is only ~25%. That is what improvement 3 fixes.
- Total-latency framing (do not lose this): buffer latency is 2.7 ms
  at 128 vs 10.7 ms at 512, but total latency is 97 ms (DF3_ll) /
  117 ms (DF3). Fixing 128 saves ~8 ms of ~105 ms. The winnable
  live case is non-monitored paths (stream/broadcast, record while
  monitoring dry), where xruns -- not latency -- are the enemy.

### 1. Native CPU flags for the libDF build

`third_party/build-libdf.sh` now exports
`RUSTFLAGS="-C target-cpu=native"` unless a non-empty RUSTFLAGS is
already set (a distributor wanting a portable binary overrides with
e.g. `-C target-cpu=x86-64`). Tempered expectation: tract's matmul
kernels already select AVX2/FMA at runtime (verified in tract-linalg
0.21.4 source via `is_x86_feature_detected`), so this helps the
surrounding scalar code only -- a few percent, measured below, not
a fix for the 128 deadline. The flag rebuilds libDF on the next
install run (cargo fingerprint change, one-time cost); Jay's machine
gets native-for-his-CPU automatically.

Measured on the build VM (denoise_wav, combo drill, block 512, wall
time for 45.9 s of audio, including model load + file I/O):
before (plain --release): DF3 5.19 s, DF3_ll 10.40 s;
after (native flags): DF3 4.81 s (-7%), DF3_ll 9.92 s (-5%).
Small, as expected -- kept anyway (see the divergence note below).

Correction (same day, before pushing): the "divergence" first reported
here was a measurement error, not a native-flags effect -- owned
plainly. The old combo file compared against was a run_drills output
(hum filter ON, 60 Hz notches), while the new file was a manual run
with hum OFF. Removing a -40 dB hum tone changes 95% of samples;
that, not tract numerics, was the 95% / corr-0.94 difference. The
clean comparison (same args both sides) shows: current engine +
native lib is BIT-IDENTICAL to branch engine + native lib on the
combo (hum off), and combo with hum on is bit-identical across
blocks 128/512/1024. The hiss drill is unchanged by the native build
(+53.4 / -0.2 before and after), and the hum drill's floor metric
flipping to -240 dB is the silence gate (improvement 2), not the
native build: after the notch removes the hum, pause frames are
near-silent, the gate pushes exact zeros where the old engine kept
DF's tiny residual (branch-vs-current hum diff: max 83 LSB, diff
RMS -78 dBFS -- inaudible). So the native build's measured effect
is timing only (-7% / -5% above), within drill resolution. Lesson
kept: always compare identical harness args (hum on/off bit), and
never quote a -240 dB floor as a reduction -- it is all-zero
windows hitting the meter bottom, not a gain figure.

### 2. Silence gating + bypass-settled skip (engine, `processDfFrame`)

Two ways inference is skipped when its output is unused or
inaudible. Both live in `processDfFrame()` (factored out of
`process()` so the worker in improvement 3 reuses it), both keep
the wet stream's frame count and timing intact, and neither changes
any parameter, the TTL, or reported latency.

- Silence gate: a DF frame whose RMS is below -60 dBFS
  (`kSilentGateRms` = 0.001) is not worth a full inference -- the
  model outputs ~zero on such frames anyway. But the model's output
  lags its input by a few frames (lookahead + STFT), so the first
  8 silent frames (`kSilentGateDelayFrames`, 80 ms) are still
  processed normally -- their outputs carry the speech tail -- and
  only subsequent silent frames are skipped, pushing zeros so
  wet/dry alignment never shifts. A non-silent frame resets the
  count and resumes at once. The decision depends only on frame
  content, never on host block size, so block invariance is
  unaffected (verified by the suite). Savings depend on material:
  large for speech with pauses or idle inputs, ~zero for continuous
  noise. `gatedFrames()` exposes the skip count for tests.
- Bypass-settled skip: when bypass is on, the mix has settled at
  exactly 0, and Delta Monitor is off, the wet output is multiplied
  by zero -- inference is pure waste, so it is skipped (nothing
  pushed; the render loop's starvation fallback supplies delayed
  dry, which IS the bypass output). Skipped only in that exact
  state: during the 10 ms mix ramp (either direction) inference
  still runs, and with Delta Monitor on it still runs (bypass +
  delta must output silence, and the fallback would output dry
  instead -- wrong). The pop already happened, so no input backlog
  accumulates and un-bypassing resumes on current audio.

Tests: new `tests/unit/test-silence-gate.cpp` (real speech from the
clean drill, 2 s speech + 2 s silence + same speech, block 512):
tail preserved (last 80 ms of speech output is real signal),
deep silence silent, speech resumes, gatedFrames() > 100 for the
2 s silence, and a continuous hiss+speech control never gates.
Existing tests unchanged in behavior: the silence test still
passes (gated zeros are zeros), the bypass test still passes
(fallback dry equals delayed dry), block invariance still passes
(gate decisions are content-driven). Drill effect, measured:
the hum drill's floor metric flips to -240 dB (meter bottom)
because post-notch pause frames now go to exact zeros instead of
DF's tiny residual (branch-vs-current hum diff: max 83 LSB, diff
RMS -78 dBFS -- inaudible); hiss/combo/quiet numbers otherwise
unchanged, gates still pass.

### 3. Worker-thread async inference (the 128 fix)

The structural fix. DF inference for one 480-sample frame costs
more than a 128-sample block's 2.7 ms deadline, so synchronous
inference xruns at 128 even at ~25% average CPU. Now the audio
thread never runs inference: each channel gets a worker thread
that runs the same frames in the same order (via `processDfFrame`,
shared with the sync path), while the audio thread only resamples,
shuffles rings, and renders. This adds NO reported latency and no
parameter/TTL change: the worker's time budget is the existing
`wetStart` slack (4096 samples = 85 ms at 48 kHz) -- a frame's input
arrives ~117 ms before its render position, so the worker must keep
up on average, not per block. If it ever falls behind, the render
loop falls back to delayed dry (a brief un-denoised moment), never
a dropout or a crash. If the worker stalls long enough to fill the
16384-sample input ring, that block's model samples are dropped
(same graceful fallback, documented at the push site).

Mechanics, for the next person touching this:

- Rings are SPSC lock-free (`dsp/RingBuffer.hpp`: atomic size_,
  producer-owned write_, consumer-owned read_). Audio produces
  input / consumes output; the worker consumes input / produces
  output. `reset()` is NOT thread-safe and is only called with
  the worker stopped (init / setModel / reset all stop it first).
- The worker waits on a condition variable for (input frame
  available AND output room available), with a 5 ms timeout
  backstop so a missed notify delays a frame instead of stalling
  it. The audio thread notifies after pushing input and after
  popping output (room freed); it never sleeps on the worker.
- `dfState_` is worker-owned while the worker runs, so the
  post-filter/attenuation setters defer through atomics when the
  worker is up (applied at the top of the worker loop) and apply
  directly when it is down (sync mode, activate-time re-apply).
  `setModel`/`reset` stop the worker across the load and restart
  it if it was up (including on load failure, where the previous
  state is preserved). The plugin starts the worker in
  `activate()` (after init + param re-apply) and stops it in
  `deactivate()`; the engine destructor stops it as a backstop.
  start/stop are not thread-safe against each other -- in practice
  the host serializes activate/deactivate/setModel/reset.
- Tests and the offline harness never start the worker, so they
  keep the synchronous path and all existing tests are unchanged
  in behavior. New `tests/unit/test-async.cpp` proves the async
  path is the same path: same input (3 s speech + 1 s trailing
  silence, block 512) through a worker engine (drain-waited after
  every block) and a sync engine must be BIT-IDENTICAL, with
  workerFrames() > 0 proving the worker actually ran.

Validation (full `validate-build.sh`, Oct 31, after all three):
ALL GREEN -- drills 4/0 (hiss +53.4/-0.2, hum floor meter-bottom
via the gate, combo +58.7/-0.8 report-only, quiet meter-bottom),
unit tests 7/7 (async bit-identical to sync over 192000 samples,
worker handled 400 frames; silence-gate tail/deep/resume/control
all as specified; block invariance, bypass, latency, samplerate,
silence all pass), latency script both models exact (5616/4656),
param contract 10/10, LV2 smoke OK. The async test caught one real
bug before this run: `waitWorkerIdle` first waited for an EMPTY
input ring, but the audio thread pushes ~512 samples per block
while the worker consumes 480-sample frames, so a partial-frame
leftover (< 480) is normal and the wait timed out on nearly every
block. Idle now means "no complete frame remains"
(available() < frameLen_), and the test fails fast on a real stall
instead of burning a timeout per block.
What this does NOT prove: real-time behavior at 128 on Jay's
machine (this VM has no real-time host measurement). The 128
verdict needs his Ardour run: if xruns are gone at 128 on both
models, the deadline problem is solved; if DF3_ll still xruns,
its per-frame cost exceeds even the worker's average-throughput
budget on his machine, and the remaining levers are the 48k
resampler bypass (above) or a smaller model (none available).
