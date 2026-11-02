// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: output trim applies to the wet signal only.
// Trim is makeup gain for the processed signal -- the dry signal was
// never processed, so it must never be trimmed. That is what makes
// bypass a valid A/B reference: with trim on the bypass path, turning
// the output trim up raises the reference too, and gain-matching the
// processed output against the original becomes impossible (the gap
// never closes). Three pins, all at settled state (first second
// skipped: mix ramp + wet-start prefix):
//   1. Bypass + output trim +6 dB: output is bit-identical to delayed
//      dry -- the trim must not move the reference at all.
//   2. Full wet, trim +6 dB vs trim 0 dB: output scales by exactly
//      10^(6/20) -- the trim still works on the processed signal.
//   3. 50% mix, trim +6 dB: output equals dry-half untrimmed plus
//      wet-half trimmed -- so a gain-matched full-wet output stays
//      level-matched across the whole mix range.
#include <cmath>
#include <cstdio>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

namespace {

bool loadEngine(DenoiserEngine& engine, uint32_t rate, const std::string& md) {
  return engine.init(rate, md, modelIndex()) && engine.modelLoaded();
}

}  // namespace

int main() {
  const uint32_t rate = 48000;
  const std::vector<float> sig = makeNoise(rate * 5, 4242);

  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL trim-bypass: NOISE_ORDNANCE_MODEL_DIR not set (a missing "
                "model must not silently pass as dry audio)\n");
    return 1;
  }

  // Run 1: bypass + output trim +6 dB.
  DenoiserEngine engBypass;
  if (!loadEngine(engBypass, rate, md)) {
    std::printf("FAIL trim-bypass: model did not load from %s\n", md.c_str());
    return 1;
  }
  engBypass.setBypassed(true);
  engBypass.setOutputTrim(6.0f);
  std::vector<float> outBypass;
  runEngine(engBypass, sig, outBypass, 256);
  const uint32_t delay = engBypass.latencySamples();

  // Run 2: full wet, trim 0 dB.
  DenoiserEngine engWet0;
  if (!loadEngine(engWet0, rate, md)) {
    std::printf("FAIL trim-bypass: model did not load from %s\n", md.c_str());
    return 1;
  }
  engWet0.setReduction(1.0f);
  std::vector<float> outWet0;
  runEngine(engWet0, sig, outWet0, 256);

  // Run 3: full wet, trim +6 dB.
  DenoiserEngine engWet6;
  if (!loadEngine(engWet6, rate, md)) {
    std::printf("FAIL trim-bypass: model did not load from %s\n", md.c_str());
    return 1;
  }
  engWet6.setReduction(1.0f);
  engWet6.setOutputTrim(6.0f);
  std::vector<float> outWet6;
  runEngine(engWet6, sig, outWet6, 256);

  // Run 4: 50% mix, trim +6 dB.
  DenoiserEngine engMix;
  if (!loadEngine(engMix, rate, md)) {
    std::printf("FAIL trim-bypass: model did not load from %s\n", md.c_str());
    return 1;
  }
  engMix.setReduction(0.5f);
  engMix.setOutputTrim(6.0f);
  std::vector<float> outMix;
  runEngine(engMix, sig, outMix, 256);

  const size_t skip = delay > rate ? delay : rate;  // past priming + delay fill
  const float factor = std::pow(10.0f, 6.0f / 20.0f);
  const float peakWet = peak(outWet6);

  size_t checked = 0, bypassMatched = 0;
  double maxWetErr = 0.0, maxMixErr = 0.0;
  for (size_t i = skip; i < sig.size(); ++i) {
    ++checked;
    if (i >= delay && outBypass[i] == sig[i - delay])
      ++bypassMatched;
    const double wetErr =
        std::fabs(static_cast<double>(outWet6[i]) -
                  static_cast<double>(outWet0[i]) * factor);
    if (wetErr > maxWetErr)
      maxWetErr = wetErr;
    if (i >= delay) {
      const double expectedMix =
          0.5 * static_cast<double>(sig[i - delay]) +
          0.5 * static_cast<double>(outWet6[i]);
      const double mixErr = std::fabs(static_cast<double>(outMix[i]) - expectedMix);
      if (mixErr > maxMixErr)
        maxMixErr = mixErr;
    }
  }

  const double tol = 1e-4 * std::max(1e-6, static_cast<double>(peakWet));
  const bool ok = allFinite(outBypass) && allFinite(outWet6) && allFinite(outMix) &&
                  delay > 0 && bypassMatched == checked && maxWetErr <= tol &&
                  maxMixErr <= tol;
  if (ok) {
    std::printf("PASS trim-bypass: bypass+6dB %zu/%zu bit-identical to delayed "
                "dry; wet scales by %.4f (max err %.2e); 50%% mix splits dry "
                "untrimmed + wet trimmed (max err %.2e)\n",
                bypassMatched, checked, factor, maxWetErr, maxMixErr);
    return 0;
  }
  std::printf("FAIL trim-bypass: bypass matched %zu/%zu, wet max err %.2e, "
              "mix max err %.2e (tol %.2e), delay %u\n",
              bypassMatched, checked, maxWetErr, maxMixErr, tol, delay);
  return 1;
}
