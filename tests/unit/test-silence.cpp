// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: silence in -> silence out, no NaNs.
// RNNoise's silence path plus the engine's dry/wet mix must not invent
// signal: 5 seconds of digital silence through default settings must come
// out silent (and finite -- a NaN anywhere fails, even if it averages out).
#include <cstdio>

#include "test-common.hpp"

using namespace unit_test;

int main() {
  const uint32_t rate = 48000;
  const std::vector<float> in(rate * 5, 0.0f);

  DenoiserEngine engine;
  engine.init(rate);
  std::vector<float> out;
  runEngine(engine, in, out, 512);

  const float m = maxAbs(out);
  const bool finite = allFinite(out);
  if (finite && m < 1e-6f) {
    std::printf("PASS silence: max abs output %.6g, all finite\n", m);
    return 0;
  }
  std::printf("FAIL silence: max abs output %.6g, all finite %d\n", m, (int)finite);
  return 1;
}
