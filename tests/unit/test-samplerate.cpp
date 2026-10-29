// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: sample-rate invariance.
// RNNoise runs at a fixed 48 kHz internally, so the engine resamples to
// and from the host rate. The same logical signal at 44.1, 48, and 96 kHz
// must behave consistently: same output length as input, finite output,
// output level within a generous band of the 48 kHz reference, and a
// reported latency that scales with the rate (the wet path is a fixed
// time, not a fixed sample count). Gates are loose on purpose -- the
// resamplers legitimately color the signal slightly -- but a broken
// resampler path (silence, explosion, wild level change) fails loudly.
#include <cstdio>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

namespace {

struct RateResult {
  double rate = 0;
  uint32_t latency = 0;
  float outRms = 0;
  float outPeak = 0;
  bool finite = false;
  bool lengthOk = false;
};

}  // namespace

int main() {
  const std::vector<float> ref = makeNoise(48000 * 2, 777);
  const double rates[] = {44100.0, 48000.0, 96000.0};
  RateResult results[3];
  bool ok = true;

  for (int k = 0; k < 3; ++k) {
    const std::vector<float> sig = resampleFrom48k(ref, rates[k]);
    DenoiserEngine engine;
    engine.init(rates[k]);
    std::vector<float> out;
    runEngine(engine, sig, out, 512);
    results[k].rate = rates[k];
    results[k].latency = engine.latencySamples();
    results[k].outRms = rms(out);
    results[k].outPeak = peak(out);
    results[k].finite = allFinite(out);
    results[k].lengthOk = (out.size() == sig.size());
    if (!results[k].finite || !results[k].lengthOk)
      ok = false;
  }

  const RateResult& r48 = results[1];
  for (int k = 0; k < 3; ++k) {
    if (k == 1)
      continue;
    const float rmsRatio = results[k].outRms / r48.outRms;
    const float peakRatio = results[k].outPeak / r48.outPeak;
    const double expectedLatency = r48.latency * rates[k] / 48000.0;
    const double latencyErr =
        std::fabs((double)results[k].latency - expectedLatency) / expectedLatency;
    std::printf("  %.0f Hz: latency %u (expected ~%.0f, err %.1f%%), "
                "rms ratio %.3f, peak ratio %.3f\n",
                rates[k], results[k].latency, expectedLatency, latencyErr * 100.0,
                rmsRatio, peakRatio);
    if (rmsRatio < 0.7f || rmsRatio > 1.4f)
      ok = false;
    if (peakRatio < 0.8f || peakRatio > 1.25f)
      ok = false;
    if (latencyErr > 0.10)
      ok = false;
  }

  if (ok) {
    std::printf("PASS samplerate: 44.1/48/96 kHz consistent (48k ref: latency %u, "
                "rms %.6g, peak %.6g)\n",
                r48.latency, r48.outRms, r48.outPeak);
    return 0;
  }
  std::printf("FAIL samplerate: see per-rate numbers above\n");
  return 1;
}
