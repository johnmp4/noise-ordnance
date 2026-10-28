// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: impulse in -> peak at the expected latency.
// The engine measures its own wet-path latency at init() with an impulse
// probe and reports it via latencySamples(), which the plugin hands to the
// host for delay compensation. This test re-runs the same probe from the
// outside and checks the observed wet-path peak lands exactly where the
// reported latency says it should -- if the two ever disagree, the host
// compensates by the wrong amount and dry/wet alignment breaks.
// The expected value comes from latencySamples(), never a hardcoded guess.
#include <cmath>
#include <cstdio>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

int main() {
  const uint32_t rate = 48000;
  const uint32_t settle = rate;        // 1 s of zeros (mix/pipeline settle)
  const uint32_t tail = rate / 2;      // 0.5 s tail to catch the peak
  const uint32_t total = settle + 1 + tail;
  std::vector<float> in(total, 0.0f), out;
  in[settle] = 1.0f;

  DenoiserEngine engine;
  engine.init(rate);
  engine.setReduction(1.0f);  // 100% wet: measure the wet path only
  runEngine(engine, in, out, 128);

  const uint32_t latency = engine.latencySamples();
  float peakVal = 0.0f;
  uint32_t peakAt = 0;
  for (uint32_t i = settle + 64; i < total; ++i) {
    const float a = std::fabs(out[i]);
    if (a > peakVal) {
      peakVal = a;
      peakAt = i;
    }
  }
  const uint32_t expected = settle + latency;
  if (peakVal > 1e-4f && peakAt == expected) {
    std::printf("PASS impulse: latency %u samples, peak %.6g at %u (expected %u)\n",
                latency, peakVal, peakAt, expected);
    return 0;
  }
  std::printf("FAIL impulse: latency %u, peak %.6g at %u (expected %u)\n",
              latency, peakVal, peakAt, expected);
  return 1;
}
