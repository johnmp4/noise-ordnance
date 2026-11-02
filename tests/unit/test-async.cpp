// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: async inference (worker thread, Oct 31, 2026) must
// produce bit-identical output to the synchronous path. Same input
// (3 s of real speech + 1 s of trailing silence, block 512) through
// two engines, one with the worker started and one without. The async
// run waits for worker drain after every block, so the render loop
// never starves and never falls back to delayed dry -- any difference
// would mean the worker changed frame order, content, or timing.
// workerFrames() > 0 proves the worker actually ran (not a silent
// fallback to sync). The trailing silence flushes the delayed wet tail
// so the comparison covers the full pipeline, not just the prefix.
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
    std::printf("FAIL async: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return 1;
  }
  const char* drills = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drills == nullptr || drills[0] == '\0') {
    std::printf("FAIL async: NOISE_ORDNANCE_DRILLS_DIR not set\n");
    return 1;
  }
  std::vector<float> clean;
  if (!readWavMono16((std::string(drills) + "/paint-drying-clean.wav").c_str(), clean)) {
    std::printf("FAIL async: could not read clean drill WAV\n");
    return 1;
  }
  std::vector<float> in(clean.begin(), clean.begin() + 144000);
  in.insert(in.end(), 48000, 0.0f);  // trailing silence: flush the tail

  DenoiserEngine sync;
  if (!sync.init(48000.0, md, modelIndex()) || !sync.modelLoaded()) {
    std::printf("FAIL async: sync model did not load\n");
    return 1;
  }
  std::vector<float> outSync;
  runEngine(sync, in, outSync, 512);

  DenoiserEngine async;
  if (!async.init(48000.0, md, modelIndex()) || !async.modelLoaded()) {
    std::printf("FAIL async: async model did not load\n");
    return 1;
  }
  async.startWorker();
  std::vector<float> outAsync(in.size(), 0.0f);
  for (size_t done = 0; done < in.size();) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(512, in.size() - done));
    async.process(in.data() + done, outAsync.data() + done, n);
    if (!async.waitWorkerIdle(5000)) {
      // Fail fast: a stalled worker would otherwise burn the full
      // timeout on every remaining block (minutes of nothing).
      std::printf("FAIL async: worker did not drain after block at %zu\n", done);
      async.stopWorker();
      return 1;
    }
    done += n;
  }
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
    std::printf("PASS async: bit-identical to sync over %zu samples, "
                "worker handled %llu frames\n",
                in.size(), (unsigned long long)frames);
    return 0;
  }
  std::printf("FAIL async: differ %zu samples (max diff %.6g), "
              "worker frames %llu, finite %d\n",
              differ, maxDiff, (unsigned long long)frames,
              (int)allFinite(outAsync));
  return 1;
}
