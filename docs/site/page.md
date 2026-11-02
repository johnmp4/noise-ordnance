---
title: Noise Ordnance
tagline: A neural denoiser you can steer.
tagline_em: DeepFilterNet's brain, your hands on the controls.
formats: LV2 · VST3
theme_bg: #050a06
theme_panel: #0a140c
theme_panel_edge: #1d3320
theme_text: #c8f0c8
theme_muted: #6a9a6e
theme_dim: #3f5a42
theme_amber: #4ade80
theme_led: #33ff66
stats:
  - "+53.4 dB | Hiss reduction, gated drill"
  - "+58.7 dB | Combo drill, noisy speech"
  - "10 | Parameters, each one earning its place"
links:
  - "Documentation | https://plugins.johnpeterman.me/noise-ordnance"
  - "Source | https://github.com/johnmp4/noise-ordnance"
  - "Releases | https://github.com/johnmp4/noise-ordnance/releases"
  - "Issues | https://github.com/johnmp4/noise-ordnance/issues"
---

A free, open-source neural denoiser for recorded speech: podcast
dialogue, voiceover, audiobook narration, video dialogue. On Linux
(LV2) and anywhere VST3 runs. Instead of a black box, you get ten
controls over how hard it works, and a monitor that lets you hear
exactly what's being removed.

Built on [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet),
with DF3 as the default model and DF3_ll on a Low Latency Model
switch. A traditional notch-filter bank handles mains hum ahead of
the model. Latency is about 100 ms (5616 samples DF3, 4656 DF3_ll
at 48 kHz), reported to the host for delay compensation, so this is
a plugin for tracks you have already recorded, not for monitoring
yourself live.

Tune it by ear: enable Delta Monitor, set Aggressiveness to full,
and listen to what's being removed. Lower Aggressiveness until the
voice in the delta is faint. That's your sweet spot.

## What it does

- Hiss and broadband noise, via the DeepFilterNet model
- Hum, via a notch-filter bank ahead of the model (50 or 60 Hz mains)
- Reduction: the wet/dry mix, with smoothed transitions so automation doesn't zip
- Aggressiveness: a cap on how deep the cuts go, the knob that keeps the metallic edge out
- Postfilter Strength: DeepFilterNet's perceptual cleanup stage (0 = off)
- Delta Monitor: listen to dry-minus-wet, hear exactly what's removed
- Bypass that keeps timing: delayed dry at full reported latency, so A/B comparisons don't shift

## Before / after

Same noisy clip, raw and through Noise Ordnance at default settings.

<!-- PLACEHOLDER: audio files pending Jay's follow-up commit (cleaned-up
     combo drill file + his before/after export). Filenames below are
     provisional. The srcs point at the tests directory by relative
     path; the site build currently copies only top-level media from
     the page's own directory, so at go-live either the build needs a
     relative-path rule or the audio gets copied into the page dir.
     Decision deferred. -->
<audio controls>
  <source src="../../tests/drills/paint-drying-combo.wav" type="audio/wav">
  Before: raw noisy clip.
</audio>
<audio controls>
  <source src="./paint-drying-combo-denoised.wav" type="audio/wav">
  After: through Noise Ordnance.
</audio>

## Who it's for

- Podcast, voiceover, audiobook: recorded tracks, where latency is irrelevant and one instance on a desktop is nothing.
- Video dialogue: location audio cleanup in Ardour, REAPER, Audacity 4, and VST3 editors on Windows and macOS.
- Streamers with a DAW chain: mic through Ardour or REAPER into OBS, where the built-in suppression isn't enough for heavy or non-stationary noise.
- Not for: live monitoring, in-ear, front of house. About 100 ms rules those out, plainly.

## Under the hood

- Open source, MIT. Source, releases, and issues are linked below.
- Runs at 128-sample blocks in Ardour and Carla: inference lives on a worker thread, so the audio thread stays clear of the model's deadline.
- Tested, not just built: the unit suite covers block-size invariance, exact latency, model-switch races, and bypass alignment, and the drill suite gates hiss and hum on real speech, not tones.
- Install today is a source build: a Rust toolchain plus model downloads (DF3 8 MB, DF3_ll 36 MB). Prebuilt bundles are the next packaging step, not yet shipped.
