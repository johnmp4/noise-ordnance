// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: model switches under concurrent audio must not
// crash or misalign the stream. setModel() and the audio thread share
// dfState_: without protection the switch could free it while the
// audio thread's sync fallback (worker down during the switch) is
// inside df_process_frame, corrupting the stream or aborting in tract
// on freed graph memory. The guard is a reader-writer lock
// (stateMutex_): the audio thread takes shared (try, never blocking)
// and the switch takes exclusive only for the fast swap + pipeline reset.
//   This test reproduces the live shape: the audio thread processes
// 128-blocks with the worker on while a second thread fires model
// switches by block count, exactly like a host calling
// setParameterValue during playback. It asserts the run completes
// (a crash fails the binary outright), the worker actually ran, the
// output is finite, and the final steady window -- seconds after the
// last switch, so no switch transient is inside it -- correlates at
// exactly the final model's latency. A misaligned stream (or a
// stream stuck in passthrough, which correlates at lag 0, not at the
// latency) fails the correlation check.
//   Pacing note: the audio thread drains the worker after EVERY
// block (waitWorkerIdle), exactly like test-async. This keeps the
// test in the live regime: a live host delivers blocks at 1x and the
// worker runs ahead of real time, so the ring stays near empty and
// the wet stream never starves. Starvation matters because the
// render loop is FIFO with no absolute-time anchor for wet: any
// position where wet is unavailable renders dry instead, and the
// late-arriving wet then renders late by the starved amount -- a
// PERMANENT wet/dry offset until the next pipeline reset. A fully
// unpaced audio thread (or drains every 32 blocks, measured during
// development) outruns the worker, starves repeatedly, and lands at
// a constant wrong lag (observed: 6544, and 17744 fully unpaced,
// instead of 5616) -- a test-harness artifact, not a live behavior,
// since live the worker never falls behind. The per-block drain
// keeps the test measuring the switch race, not queueing delay,
// without wall-clock pacing it to 1x.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "test-common.hpp"

using namespace unit_test;

namespace {

// Same local WAV reader as the other drill-based tests (test-async,
// test-silence-gate): mono 16-bit, data chunk located by tag.
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

float corrAtLag(const std::vector<float>& out, const std::vector<float>& in,
                size_t w0, size_t win, int lag) {
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (size_t j = 0; j < win; ++j) {
    const double a = out[w0 + j];
    const double b = in[w0 + j - static_cast<size_t>(lag)];
    dot += a * b;
    na += a * a;
    nb += b * b;
  }
  return static_cast<float>(dot / (std::sqrt(na * nb) + 1e-30));
}

struct Event {
  size_t block;
  int model;
};

}  // namespace

int main() {
  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL model-switch-race: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return 1;
  }
  const char* drills = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drills == nullptr || drills[0] == '\0') {
    std::printf("FAIL model-switch-race: NOISE_ORDNANCE_DRILLS_DIR not set\n");
    return 1;
  }
  std::vector<float> clean;
  if (!readWavMono16((std::string(drills) + "/paint-drying-clean.wav").c_str(),
                     clean)) {
    std::printf("FAIL model-switch-race: could not read clean drill WAV\n");
    return 1;
  }
  // 8 s of speech: switches in the first ~5.5 s, final window 7-8 s
  // is steady (1.5+ s after the last switch in audio time, far beyond
  // the ~117 ms pipeline refill).
  const size_t n = 8 * 48000;
  if (clean.size() < n) {
    std::printf("FAIL model-switch-race: clean WAV too short (%zu)\n", clean.size());
    return 1;
  }
  const std::vector<float> in(clean.begin(), clean.begin() + n);

  DenoiserEngine e;
  if (!e.init(48000.0, md, 0) || !e.modelLoaded()) {
    std::printf("FAIL model-switch-race: model did not load\n");
    return 1;
  }
  e.setReduction(1.0f);
  e.startWorker();

  // Four switches, toggling DF3 -> LL -> DF3 -> LL -> DF3, so the
  // final model is DF3 (latency 5616).
  const std::vector<Event> events = {{500, 1}, {1000, 0}, {1500, 1}, {2000, 0}};

  std::vector<float> out(n, 0.0f);
  std::atomic<size_t> blocksDone{0};
  std::atomic<bool> audioDone{false};
  std::atomic<bool> drainFailed{false};
  std::thread audio([&] {
    const uint32_t block = 128;
    for (size_t done = 0, b = 0; done < in.size(); b++) {
      const uint32_t m = static_cast<uint32_t>(std::min<size_t>(block, in.size() - done));
      e.process(in.data() + done, out.data() + done, m);
      done += m;
      blocksDone.store(b + 1, std::memory_order_release);
      // Drain after every block (see pacing note): keeps the worker
      // caught up so the test measures the switch race, not wet
      // starvation shift. Switches still fire by block count while
      // audio processes blocks, so the race is genuinely exercised.
      if (!e.waitWorkerIdle(5000)) {
        drainFailed.store(true, std::memory_order_release);
        break;
      }
    }
    audioDone.store(true, std::memory_order_release);
  });
  std::thread ui([&] {
    size_t ev = 0;
    while (ev < events.size()) {
      const size_t done = blocksDone.load(std::memory_order_acquire);
      if (done >= events[ev].block || audioDone.load(std::memory_order_acquire)) {
        e.setModel(events[ev++].model);
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  });
  audio.join();
  ui.join();
  e.stopWorker();

  if (drainFailed.load()) {
    std::printf("FAIL model-switch-race: worker did not drain during run\n");
    return 1;
  }
  const uint64_t frames = e.workerFrames();
  const float corr = corrAtLag(out, in, 7 * 48000, 1 * 48000, 5616);
  if (frames > 100 && allFinite(out) && corr > 0.9f) {
    std::printf("PASS model-switch-race: 4 switches under audio, no crash; "
                "final window corr %.3f at lag 5616, worker frames %llu\n",
                corr, (unsigned long long)frames);
    return 0;
  }
  std::printf("FAIL model-switch-race: final corr %.3f (want > 0.9 at 5616), "
              "worker frames %llu, finite %d\n",
              corr, (unsigned long long)frames, (int)allFinite(out));
  return 1;
}
