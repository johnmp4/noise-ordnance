// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: reported latency matches the observed wet-path delay.
// The engine reports a measured per-model latency via latencySamples()
// (table in dsp/DenoiserEngine.cpp), which the plugin hands to the host
// for delay compensation. This test checks the observed wet-path delay
// lands where the reported latency says it should -- if the two ever
// disagree, the host compensates by the wrong amount and dry/wet
// alignment breaks. The expected value comes from latencySamples(),
// never a hardcoded guess.
// The model under test is $NOISE_ORDNANCE_MODEL_INDEX (default 0).
//
// Probe: real speech, by cross-correlation. A lone impulse does NOT
// work as a probe for DF (the model gates near-silent frames to exact
// zero, so an impulse in silence produces no output under libDF 0.5.6),
// and neither does a noise burst (DF suppresses stationary noise below
// any useful threshold). Speech is what DF passes, so speech is what
// measures it: a 5 s slice of the clean drill speech (2 s of silence
// prepended for pipeline settle, 1 s tail) is run through the engine
// at 100% wet, and the input slice is correlated against the output
// over lags 0..12000. The correlation peak lag is the true delay.
// The clean file comes from $NOISE_ORDNANCE_DRILLS_DIR (set by
// run-unit-tests.sh); the test fails clearly if it is missing, rather
// than silently passing on nothing.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test-common.hpp"

using namespace unit_test;

namespace {

// Minimal PCM16 WAV reader (mono or stereo -> mono, +-1.0). Only what
// this test needs; the full reader lives in tests/denoise_wav.cpp.
bool readWavMono(const char* path, std::vector<float>& samples, uint32_t& rate) {
  FILE* f = std::fopen(path, "rb");
  if (!f)
    return false;
  char id[4];
  uint32_t size;
  uint16_t audioFormat = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0;
  long dataPos = -1;
  uint32_t dataSize = 0;
  if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "RIFF", 4) != 0) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, 4, SEEK_CUR);
  if (std::fread(id, 1, 4, f) != 4 || std::memcmp(id, "WAVE", 4) != 0) {
    std::fclose(f);
    return false;
  }
  while (dataPos < 0) {
    if (std::fread(id, 1, 4, f) != 4)
      break;
    if (std::fread(&size, 4, 1, f) != 1)
      break;
    const long chunkStart = std::ftell(f);
    if (std::memcmp(id, "fmt ", 4) == 0) {
      uint16_t blockAlign;
      uint32_t byteRate;
      std::fread(&audioFormat, 2, 1, f);
      std::fread(&channels, 2, 1, f);
      std::fread(&sampleRate, 4, 1, f);
      std::fread(&byteRate, 4, 1, f);
      std::fread(&blockAlign, 2, 1, f);
      std::fread(&bits, 2, 1, f);
    } else if (std::memcmp(id, "data", 4) == 0) {
      dataPos = chunkStart;
      dataSize = size;
      break;
    }
    std::fseek(f, chunkStart + ((size + 1) & ~1u), SEEK_SET);
  }
  if (dataPos < 0 || audioFormat != 1 || bits != 16 || channels < 1) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, dataPos, SEEK_SET);
  const size_t frames = dataSize / (channels * 2);
  std::vector<int16_t> tmp(frames * channels);
  if (std::fread(tmp.data(), 2, frames * channels, f) != frames * channels) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  samples.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (uint16_t c = 0; c < channels; ++c)
      sum += tmp[i * channels + c];
    samples[i] = static_cast<float>((sum / channels) / 32768.0);
  }
  rate = sampleRate;
  return true;
}

}  // namespace

int main() {
  const char* drillsDir = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drillsDir == nullptr || drillsDir[0] == '\0') {
    std::printf("FAIL latency: NOISE_ORDNANCE_DRILLS_DIR not set "
                "(run-unit-tests.sh sets it)\n");
    return 1;
  }
  const std::string cleanPath =
      std::string(drillsDir) + "/paint-drying-clean.wav";
  std::vector<float> clean;
  uint32_t cleanRate = 0;
  if (!readWavMono(cleanPath.c_str(), clean, cleanRate)) {
    std::printf("FAIL latency: could not read %s\n", cleanPath.c_str());
    return 1;
  }
  if (cleanRate != 48000) {
    std::printf("FAIL latency: clean file is %u Hz, expected 48000\n", cleanRate);
    return 1;
  }
  // 5 s speech slice from 10-15 s in (continuous narration, no silence).
  const size_t sliceStart = 10 * 48000;
  const size_t sliceLen = 5 * 48000;
  if (clean.size() < sliceStart + sliceLen) {
    std::printf("FAIL latency: clean file too short (%zu samples)\n", clean.size());
    return 1;
  }
  const std::vector<float> ref(clean.begin() + sliceStart,
                               clean.begin() + sliceStart + sliceLen);
  if (rms(ref) < 0.01f) {
    std::printf("FAIL latency: speech slice is near-silent (rms %.6g) -- "
                "slice choice is wrong, not the engine\n",
                rms(ref));
    return 1;
  }

  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL latency: NOISE_ORDNANCE_MODEL_DIR not set (a missing "
                "model must not silently pass as dry audio)\n");
    return 1;
  }
  DenoiserEngine engine;
  if (!engine.init(48000.0, md, modelIndex()) || !engine.modelLoaded()) {
    std::printf("FAIL latency: model did not load from %s\n", md.c_str());
    return 1;
  }
  engine.setReduction(1.0f);  // 100% wet: measure the wet path only

  // 2 s silence (settle past the mix ramp + wet-start prefix) + slice +
  // 1 s tail. Output length matches input; the speech region in the
  // output starts at 2 s + true delay.
  const size_t pre = 2 * 48000;
  const size_t tail = 1 * 48000;
  std::vector<float> in(pre + sliceLen + tail, 0.0f);
  std::copy(ref.begin(), ref.end(), in.begin() + pre);
  std::vector<float> out;
  runEngine(engine, in, out, 256);

  const uint32_t latency = engine.latencySamples();
  // Correlate ref against out[pre + lag ...] for lags 0..12000.
  const uint32_t maxLag = 12000;
  double bestVal = -1.0;
  uint32_t bestLag = 0;
  double refEnergy = 0.0;
  for (float v : ref)
    refEnergy += static_cast<double>(v) * v;
  for (uint32_t lag = 0; lag <= maxLag; ++lag) {
    double dot = 0.0, outEnergy = 0.0;
    for (size_t i = 0; i < sliceLen; ++i) {
      const float o = out[pre + lag + i];
      dot += static_cast<double>(ref[i]) * o;
      outEnergy += static_cast<double>(o) * o;
    }
    const double norm = std::sqrt(refEnergy * outEnergy);
    const double corr = norm > 0.0 ? dot / norm : 0.0;
    if (corr > bestVal) {
      bestVal = corr;
      bestLag = lag;
    }
  }
  const int32_t diff =
      static_cast<int32_t>(bestLag) - static_cast<int32_t>(latency);
  if (bestVal < 0.5) {
    std::printf("FAIL latency: weak correlation peak %.3f at lag %u "
                "(reported %u) -- measurement untrustworthy, not a pass\n",
                bestVal, bestLag, latency);
    return 1;
  }
  if (diff >= -48 && diff <= 48) {
    std::printf("PASS latency: reported %u, correlation peak at lag %u "
                "(diff %+d samples, peak correlation %.3f)\n",
                latency, bestLag, diff, bestVal);
    return 0;
  }
  std::printf("FAIL latency: reported %u, correlation peak at lag %u "
              "(diff %+d samples, tolerance +-48, peak correlation %.3f)\n",
              latency, bestLag, diff, bestVal);
  return 1;
}
