// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: silence in -> silence out, no NaNs.
// DeepFilterNet on silence plus the engine's dry/wet mix must not invent
// signal: 5 seconds of digital silence through default settings must come
// out silent (and finite -- a NaN anywhere fails, even if it averages out).
#include <cstdio>

#include "test-common.hpp"

using namespace unit_test;

int main() {
  const uint32_t rate = 48000;
  const std::vector<float> in(rate * 5, 0.0f);

  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL silence: NOISE_ORDNANCE_MODEL_DIR not set (a missing "
                "model must not silently pass as dry audio)\n");
    return 1;
  }
  DenoiserEngine engine;
  if (!engine.init(rate, md, modelIndex()) || !engine.modelLoaded()) {
    std::printf("FAIL silence: model did not load from %s\n", md.c_str());
    return 1;
  }
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
