// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: block-size invariance.
// The engine must produce bit-identical output for the same input no
// matter what block size the host calls process() with. The output
// assembly is what makes this true, not the model core:
//   - wet rendering is gated on a fixed absolute output position
//     (wetStart_), so the gate never flips at a block-relative point;
//   - consumption subtracts only what was actually popped, so a short
//     take is never lost from the accounting;
//   - starved samples fall back to delayed dry, never undelayed dry, so
//     a starved sample differs from a wet sample by nothing but mix;
//   - the wet buffer is topped up eagerly on every call, so it never
//     sawtooths to zero in a chunk-dependent way.
// This test pins the result: same signal through fresh engines at
// several block sizes must come out bit-identical, at 48 kHz (native),
// 44.1 kHz (fractional resample ratio), and 96 kHz (2x upsample), plus
// one non-default tuning (post-filter beta + attenuation limit).
// A failure here is a real finding about the DF engine's assembly, not
// a test to delete: the Oct 2026 RNNoise engine failed this test for
// exactly these assembly reasons before it was fixed.
#include <cstdio>
#include <string>
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

bool setupEngine(DenoiserEngine& engine, double rate) {
  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL block-invariance: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return false;
  }
  if (!engine.init(rate, md, modelIndex()) || !engine.modelLoaded()) {
    std::printf("FAIL block-invariance: model did not load from %s\n", md.c_str());
    return false;
  }
  return true;
}

std::vector<float> runAt(const std::vector<float>& sig, double rate, uint32_t block,
                         bool tuned, bool& ok) {
  DenoiserEngine engine;
  if (!setupEngine(engine, rate)) {
    ok = false;
    return {};
  }
  if (tuned) {
    engine.setPostFilterBeta(0.05f);
    engine.setAttenLim(50.0f);
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
  std::vector<float> ref48 = runAt(sig48, 48000.0, 128, false, ok);
  if (!ok)
    return 1;
  for (uint32_t b : blocks) {
    if (b == 128)
      continue;
    const std::vector<float> out = runAt(sig48, 48000.0, b, false, ok);
    if (!ok)
      return 1;
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
    const std::vector<float> a = runAt(sig, rate, 128, false, ok);
    if (!ok)
      return 1;
    const std::vector<float> b = runAt(sig, rate, 1024, false, ok);
    if (!ok)
      return 1;
    const Diff d = compare(a, b);
    std::printf("  %.0f Hz block 128 vs 1024: differ %zu/%zu, max diff %.6g\n", rate,
                d.count, a.size(), d.maxAbs);
    if (d.count > 0) {
      std::printf("    first differing sample: %zu\n", d.first);
      ok = false;
    }
  }

  // 48 kHz, non-default tuning (post-filter beta + attenuation limit).
  const std::vector<float> tuned128 = runAt(sig48, 48000.0, 128, true, ok);
  if (!ok)
    return 1;
  const std::vector<float> tuned1024 = runAt(sig48, 48000.0, 1024, true, ok);
  if (!ok)
    return 1;
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
