// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: block-size invariance.
// The engine must produce bit-identical output for the same input no
// matter what block size the host calls process() with. This was broken
// until Oct 2026: output at block 128 vs 1024 differed on 99.9% of
// samples. The causes, all in the output assembly (not the DSP core):
//   - consumePhase_ subtracted the INTENDED take before capping it by
//     ring availability, so any shortfall was lost from the accounting
//     and the wet stream fell permanently behind the output timeline by
//     a chunk-dependent amount;
//   - the output-priming gate flipped at a call boundary, so it flipped
//     at a different absolute sample for different block sizes;
//   - the tail fallback emitted undelayed dry while the mix path used
//     delay-compensated dry, so every starved sample differed by the
//     full latency;
//   - the wet buffer was only refilled when completely empty, so it
//     sawtoothed down to zero and starved in chunk-dependent runs.
// The fix: subtract only what was actually popped, gate wet rendering
// on a fixed absolute output position, emit delayed dry on starvation,
// and top the wet buffer up on every call. This test pins the result:
// same signal through fresh engines at several block sizes must come
// out bit-identical, at 48 kHz (native), 44.1 kHz (fractional resample
// ratio), and 96 kHz (2x upsample), plus one non-default tuning.
#include <cstdio>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

namespace {

struct Diff {
  size_t count = 0;
  float maxAbs = 0.0f;
  size_t first = 0;
};

Diff compare(const std::vector<float>& a, const std::vector<float>& b) {
  Diff d;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    if (a[i] != b[i]) {
      if (d.count == 0)
        d.first = i;
      ++d.count;
      const float m = std::fabs(a[i] - b[i]);
      if (m > d.maxAbs)
        d.maxAbs = m;
    }
  }
  if (a.size() != b.size())
    d.count += (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
  return d;
}

std::vector<float> runAt(const std::vector<float>& sig, double rate, uint32_t block,
                         bool tuned) {
  DenoiserEngine engine;
  engine.init(rate);
  if (tuned) {
    engine.setSmoothing(0.8f);
    engine.setBandFloor(-12.0f, -12.0f, -12.0f);
    engine.setVadAdapt(0.7f);
  }
  std::vector<float> out;
  runEngine(engine, sig, out, block);
  return out;
}

}  // namespace

int main() {
  bool ok = true;

  // 48 kHz, five block sizes, defaults.
  const std::vector<float> sig48 = makeNoise(48000 * 2, 777);
  const uint32_t blocks[] = {128, 256, 512, 1024, 2048};
  std::vector<float> ref48 = runAt(sig48, 48000.0, 128, false);
  for (uint32_t b : blocks) {
    if (b == 128)
      continue;
    const std::vector<float> out = runAt(sig48, 48000.0, b, false);
    const Diff d = compare(ref48, out);
    std::printf("  48 kHz block 128 vs %u: differ %zu/%zu, max diff %.6g%s\n", b,
                d.count, ref48.size(), d.maxAbs,
                d.count > 0 ? " (first differing sample shown below)" : "");
    if (d.count > 0) {
      std::printf("    first differing sample: %zu\n", d.first);
      ok = false;
    }
  }

  // 44.1 and 96 kHz, 128 vs 1024 (resampled paths).
  const std::vector<float> ref48b = makeNoise(48000 * 2, 777);
  const double rates[] = {44100.0, 96000.0};
  for (double rate : rates) {
    const std::vector<float> sig = resampleFrom48k(ref48b, rate);
    const std::vector<float> a = runAt(sig, rate, 128, false);
    const std::vector<float> b = runAt(sig, rate, 1024, false);
    const Diff d = compare(a, b);
    std::printf("  %.0f Hz block 128 vs 1024: differ %zu/%zu, max diff %.6g\n", rate,
                d.count, a.size(), d.maxAbs);
    if (d.count > 0) {
      std::printf("    first differing sample: %zu\n", d.first);
      ok = false;
    }
  }

  // 48 kHz, non-default tuning (smoothing + floors + VAD adapt).
  const std::vector<float> tuned128 = runAt(sig48, 48000.0, 128, true);
  const std::vector<float> tuned1024 = runAt(sig48, 48000.0, 1024, true);
  const Diff dt = compare(tuned128, tuned1024);
  std::printf("  48 kHz tuned block 128 vs 1024: differ %zu/%zu, max diff %.6g\n",
              dt.count, tuned128.size(), dt.maxAbs);
  if (dt.count > 0) {
    std::printf("    first differing sample: %zu\n", dt.first);
    ok = false;
  }

  if (ok) {
    std::printf("PASS block-invariance: identical output at blocks 128/256/512/"
                "1024/2048 (48 kHz), 128/1024 (44.1/96 kHz), defaults + tuned\n");
    return 0;
  }
  std::printf("FAIL block-invariance: see per-case numbers above\n");
  return 1;
}
