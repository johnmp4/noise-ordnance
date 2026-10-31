// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: the silence gate (Oct 31, 2026) must save inference
// without truncating speech. Signal: 2 s of real speech, 2 s of digital
// silence, the same 2 s of speech again, at 48 kHz, block 512.
//   1. The speech tail is preserved: output for the last 80 ms of the
//      first speech segment is real denoised speech, not zeros -- the
//      gate's 8-frame delay exists exactly for this (the model's
//      lookahead means output lags input by a few frames, so gating
//      immediately at silence onset would cut the tail).
//   2. Deep silence stays silent (gated zeros, not invented signal).
//   3. Speech resumes after the silence (the gate resets on the first
//      non-silent frame; the model's history gap is silent frames, so
//      resumption is clean).
//   4. The gate actually engages: gatedFrames() > 100 for the 2 s
//      silence (about 200 frames, minus the 8-frame delay).
//   5. Control: 4 s of continuous hiss+speech never gates (RMS far
//      above the -60 dBFS threshold throughout).
// The speech segment is selected, not assumed: it must start and end
// with real energy, so the tail/resumption windows cannot land in a
// natural pause and fail for the wrong reason.
#include <cstdio>
#include <cstring>

#include "test-common.hpp"

using namespace unit_test;

namespace {

bool readWavMono16(const char* path, std::vector<float>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f)
    return false;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> buf(static_cast<size_t>(sz));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  // Find the data chunk; require mono 16-bit (the drill files are).
  size_t pos = 12;
  while (pos + 8 <= buf.size()) {
    const uint32_t chunkSize = static_cast<uint32_t>(buf[pos + 4]) |
                               (static_cast<uint32_t>(buf[pos + 5]) << 8) |
                               (static_cast<uint32_t>(buf[pos + 6]) << 16) |
                               (static_cast<uint32_t>(buf[pos + 7]) << 24);
    if (std::memcmp(buf.data() + pos, "data", 4) == 0) {
      const size_t n = chunkSize / 2;
      out.resize(n);
      for (size_t i = 0; i < n; ++i) {
        int16_t v = 0;
        std::memcpy(&v, buf.data() + pos + 8 + 2 * i, 2);
        out[i] = v / 32768.0f;
      }
      return true;
    }
    pos += 8 + chunkSize + (chunkSize & 1);
  }
  return false;
}

float windowMax(const std::vector<float>& v, size_t begin, size_t end) {
  float m = 0.0f;
  for (size_t i = begin; i < end && i < v.size(); ++i)
    m = std::max(m, std::fabs(v[i]));
  return m;
}

}  // namespace

int main() {
  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL silence-gate: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return 1;
  }
  const char* drills = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drills == nullptr || drills[0] == '\0') {
    std::printf("FAIL silence-gate: NOISE_ORDNANCE_DRILLS_DIR not set\n");
    return 1;
  }
  std::vector<float> clean, hiss;
  if (!readWavMono16((std::string(drills) + "/paint-drying-clean.wav").c_str(), clean) ||
      !readWavMono16((std::string(drills) + "/paint-drying-hiss.wav").c_str(), hiss)) {
    std::printf("FAIL silence-gate: could not read drill WAVs from %s\n", drills);
    return 1;
  }

  // Select a 2 s speech window that starts and ends with energy.
  const size_t seg = 96000;
  size_t start = 0;
  bool found = false;
  for (size_t s = 0; s + seg <= clean.size(); s += 48000) {
    const float head = windowMax(clean, s, s + 24000);
    const float tail = windowMax(clean, s + seg - 4800, s + seg);
    if (head > 0.05f && tail > 0.1f) {
      start = s;
      found = true;
      break;
    }
  }
  if (!found) {
    std::printf("FAIL silence-gate: no energetic 2 s speech window in clean file\n");
    return 1;
  }

  std::vector<float> in;
  in.reserve(seg * 3);
  in.insert(in.end(), clean.begin() + start, clean.begin() + start + seg);
  in.insert(in.end(), seg, 0.0f);
  in.insert(in.end(), clean.begin() + start, clean.begin() + start + seg);

  DenoiserEngine engine;
  if (!engine.init(48000.0, md, modelIndex()) || !engine.modelLoaded()) {
    std::printf("FAIL silence-gate: model did not load from %s\n", md.c_str());
    return 1;
  }
  std::vector<float> out;
  runEngine(engine, in, out, 512);
  const uint32_t lat = engine.latencySamples();

  const float tail = windowMax(out, lat + seg - 3840, lat + seg);
  const float deep = windowMax(out, lat + seg + 48000, lat + seg + 96000);
  const float resume = windowMax(out, lat + 2 * seg, lat + 2 * seg + 24000);
  const uint64_t gated = engine.gatedFrames();
  const bool finite = allFinite(out);

  // Control: continuous hiss+speech must never gate.
  DenoiserEngine ctl;
  if (!ctl.init(48000.0, md, modelIndex()) || !ctl.modelLoaded()) {
    std::printf("FAIL silence-gate: control model did not load\n");
    return 1;
  }
  std::vector<float> ctlIn(hiss.begin(), hiss.begin() + 192000);
  std::vector<float> ctlOut;
  runEngine(ctl, ctlIn, ctlOut, 512);
  const uint64_t ctlGated = ctl.gatedFrames();

  if (finite && tail > 0.05f && deep < 1e-3f && resume > 0.05f &&
      gated > 100 && ctlGated == 0) {
    std::printf("PASS silence-gate: tail %.4g, deep-silence %.4g, resume %.4g, "
                "gated %llu frames, control gated %llu (latency %u)\n",
                tail, deep, resume, (unsigned long long)gated,
                (unsigned long long)ctlGated, lat);
    return 0;
  }
  std::printf("FAIL silence-gate: tail %.4g (want >0.05), deep %.4g (want <1e-3), "
              "resume %.4g (want >0.05), gated %llu (want >100), control %llu "
              "(want 0), finite %d, latency %u\n",
              tail, deep, resume, (unsigned long long)gated,
              (unsigned long long)ctlGated, (int)finite, lat);
  return 1;
}
