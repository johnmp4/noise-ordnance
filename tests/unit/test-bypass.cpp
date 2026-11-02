// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: bypass is bit-identical to dry.
// With bypass engaged from the start, the engine must pass the dry signal
// through untouched: no denoising, no artifacts, no level change. One
// qualification, and it matters: the engine's dry path is delay-compensated
// by latencySamples() so dry/wet stay aligned in the mix (the host
// compensates the same amount), so the bypassed output equals the dry
// input delayed by exactly latencySamples() -- same sample values, D
// positions later. The first second is skipped: before the pipeline
// settles, priming passes current input undelayed, which is also dry but
// not the steady-state path this test pins down.
#include <cstdio>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

int main() {
  const uint32_t rate = 48000;
  const std::vector<float> sig = makeNoise(rate * 5, 999);

  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL bypass: NOISE_ORDNANCE_MODEL_DIR not set (a missing "
                "model must not silently pass as dry audio)\n");
    return 1;
  }
  DenoiserEngine engine;
  if (!engine.init(rate, md, modelIndex()) || !engine.modelLoaded()) {
    std::printf("FAIL bypass: model did not load from %s\n", md.c_str());
    return 1;
  }
  engine.setBypassed(true);
  std::vector<float> out;
  runEngine(engine, sig, out, 256);

  const uint32_t delay = engine.latencySamples();
  const size_t skip = delay > rate ? delay : rate;  // past priming + delay fill
  size_t checked = 0, matched = 0;
  for (size_t i = skip; i < sig.size(); ++i) {
    ++checked;
    if (i >= delay && out[i] == sig[i - delay])
      ++matched;
  }

  if (allFinite(out) && delay > 0 && matched == checked) {
    std::printf("PASS bypass: %zu/%zu settled samples bit-identical to dry "
                "delayed by latency %u\n",
                matched, checked, delay);
    return 0;
  }
  std::printf("FAIL bypass: %zu/%zu settled samples bit-identical to dry "
              "delayed by latency %u, all finite %d\n",
              matched, checked, delay, (int)allFinite(out));
  return 1;
}
