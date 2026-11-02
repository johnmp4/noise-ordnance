// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: offline-render (freewheeling) behavior (Nov 2,
// 2026). An Ardour session export calls process() as fast as the
// machine allows, not in realtime, so the worker cannot run ahead
// the way it does live. Before the fix, the render loop then fell
// back to dry for the gap and resumed with STALE wet, so the export
// smeared late wet across later speech (warbly/swishy artifact) --
// while live playback of the same session stayed clean. This test
// drives the async engine back-to-back (no per-block worker drain,
// like a freewheeling export) at 128-sample blocks and requires
// bit-identical output to the synchronous path: the bounded wait
// absorbs worker lag and the wet anchor discards stale wet, so the
// export path must not change a single sample. workerFrames() > 0
// proves the worker actually ran (not a silent fallback to sync).
// NOTE on determinism: the wait bound is 50 ms per block, ~20
// inference frames at typical speed. A stall longer than that (a
// pathologically loaded machine) would dry-fill and fail this test
// -- which is the honest signal: the engine genuinely could not
// keep up. On a normal dev machine the wait absorbs all lag and
// the test is stable.
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

}  // namespace

int main() {
  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL offline-render: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return 1;
  }
  const char* drills = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drills == nullptr || drills[0] == '\0') {
    std::printf("FAIL offline-render: NOISE_ORDNANCE_DRILLS_DIR not set\n");
    return 1;
  }
  std::vector<float> clean;
  if (!readWavMono16((std::string(drills) + "/paint-drying-clean.wav").c_str(), clean)) {
    std::printf("FAIL offline-render: could not read clean drill WAV\n");
    return 1;
  }
  std::vector<float> in(clean.begin(), clean.begin() + 144000);
  in.insert(in.end(), 48000, 0.0f);  // trailing silence: flush the tail

  DenoiserEngine sync;
  if (!sync.init(48000.0, md, modelIndex()) || !sync.modelLoaded()) {
    std::printf("FAIL offline-render: sync model did not load\n");
    return 1;
  }
  std::vector<float> outSync;
  runEngine(sync, in, outSync, 128);

  DenoiserEngine async;
  if (!async.init(48000.0, md, modelIndex()) || !async.modelLoaded()) {
    std::printf("FAIL offline-render: async model did not load\n");
    return 1;
  }
  async.startWorker();
  // Back-to-back blocks, NO per-block worker drain: this is the
  // freewheeling pattern. The old code smeared here (stale wet after
  // every starvation gap); the fix must hold bit-identity anyway.
  std::vector<float> outAsync;
  runEngine(async, in, outAsync, 128);
  async.stopWorker();
  const uint64_t frames = async.workerFrames();

  size_t differ = 0;
  float maxDiff = 0.0f;
  for (size_t i = 0; i < in.size(); ++i) {
    if (outSync[i] != outAsync[i]) {
      ++differ;
      maxDiff = std::max(maxDiff, std::fabs(outSync[i] - outAsync[i]));
    }
  }
  if (differ == 0 && frames > 0 && allFinite(outAsync)) {
    std::printf("PASS offline-render: bit-identical to sync over %zu samples "
                "with no per-block drain, worker handled %llu frames\n",
                in.size(), (unsigned long long)frames);
    return 0;
  }
  std::printf("FAIL offline-render: differ %zu samples (max diff %.6g), "
              "worker frames %llu, finite %d\n",
              differ, maxDiff, (unsigned long long)frames,
              (int)allFinite(outAsync));
  return 1;
}
