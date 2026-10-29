// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Shared helpers for the Noise Ordnance engine unit tests (tests/unit/).
// No DPF, no audio device: these tests drive dsp/DenoiserEngine directly.
// Every test binary prints one PASS/FAIL line with its measured numbers
// and exits 0 on pass, 1 on fail, so tests/run-unit-tests.sh can aggregate.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "DenoiserEngine.hpp"

namespace unit_test {

// Deterministic pseudo-random noise in [-0.8, 0.8] (xorshift32).
// Distinct samples on purpose: equality checks against a delayed copy
// are only meaningful if the signal isn't constant or periodic.
inline std::vector<float> makeNoise(size_t n, uint32_t seed) {
  std::vector<float> x(n);
  uint32_t s = seed;
  for (size_t i = 0; i < n; ++i) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    x[i] = (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) * 0.8f;
  }
  return x;
}

// Linear-interpolated resample of a 48 kHz reference to another rate.
// Gives every sample-rate run the same logical content (not the same
// sample sequence -- the rates have different sample counts by definition).
inline std::vector<float> resampleFrom48k(const std::vector<float>& ref, double rate) {
  const size_t n = static_cast<size_t>(2.0 * rate);
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) {
    const double pos = i * 48000.0 / rate;
    const size_t j = static_cast<size_t>(pos);
    const double f = pos - j;
    if (j + 1 >= ref.size())
      out[i] = ref.back();
    else
      out[i] = static_cast<float>(ref[j] * (1.0 - f) + ref[j + 1] * f);
  }
  return out;
}

// Run the whole signal through the engine in DAW-like blocks.
inline void runEngine(DenoiserEngine& engine, const std::vector<float>& in,
                      std::vector<float>& out, uint32_t block) {
  out.assign(in.size(), 0.0f);
  for (size_t done = 0; done < in.size();) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, in.size() - done));
    engine.process(in.data() + done, out.data() + done, n);
    done += n;
  }
}

inline bool allFinite(const std::vector<float>& v) {
  for (float f : v)
    if (!std::isfinite(f))
      return false;
  return true;
}

inline float maxAbs(const std::vector<float>& v) {
  float m = 0.0f;
  for (float f : v)
    m = std::max(m, std::fabs(f));
  return m;
}

inline float rms(const std::vector<float>& v) {
  double s = 0.0;
  for (float f : v)
    s += static_cast<double>(f) * f;
  return static_cast<float>(std::sqrt(s / static_cast<double>(v.size())));
}

inline float peak(const std::vector<float>& v) {
  return maxAbs(v);
}

}  // namespace unit_test
