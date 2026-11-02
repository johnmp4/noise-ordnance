// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// Engine unit test: a long bypass must not misalign the stream
// (Nov 1, 2026 bypass echo fix). Before the fix, processDfFrame
// skipped inference entirely once bypass had settled (bypassed_ &&
// mix_ == 0 && !deltaMonitor_), on the theory that wet multiplied
// by zero is pure waste and the stream would restart cleanly.
// Wrong: the gap corrupts DF's internal temporal state, and on
// un-bypass the wet stream resumes ~70 ms early and stays shifted
// until the next pipeline reset -- live, a bypass flip produced a
// persistent phasey echo, cleared only by a model switch. Short
// bypasses never triggered it because the mix needs ~112 ms to
// settle to exactly 0 (exponential ramp stall-snap), so the skip
// never engaged on a quick click.
//   This test reproduces the live shape: one engine, worker on,
// 128-blocks, full wet, bypass ON at 3 s and OFF at 6 s (long
// enough for the skip to engage pre-fix), and asserts the final
// steady window -- seconds after un-bypass, so no ramp transient
// is inside it -- still correlates at exactly the model's latency.
// Against the pre-fix engine it fails decisively (final corr
// -0.002 at 5616, best lag 2576 at corr 1.000). It also pins the
// bypass window itself: while bypass is settled the output must
// be delayed dry (corr at 5616 near 1.0 on clean input), which the
// fix preserves by discarding wet at mix_ == 0 instead of starving
// the model.
//   Pacing note: the audio thread drains the worker after EVERY
// block (waitWorkerIdle), exactly like test-async and
// test-model-switch-race. This keeps the test in the live regime:
// a live host delivers blocks at 1x and the worker runs ahead of
// real time, so the ring stays near empty and the wet stream never
// starves. Starvation matters because the render loop is FIFO with
// no absolute-time anchor for wet: any position where wet is
// unavailable renders dry instead, and the late-arriving wet then
// renders late by the starved amount -- a PERMANENT wet/dry offset
// until the next pipeline reset. The per-block drain keeps the
// test measuring the bypass gap, not queueing delay, without
// wall-clock pacing it to 1x.
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
// test-model-switch-race): mono 16-bit, data chunk located by tag.
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

}  // namespace

int main() {
  const std::string md = modelDir();
  if (md.empty()) {
    std::printf("FAIL bypass-alignment: NOISE_ORDNANCE_MODEL_DIR not set\n");
    return 1;
  }
  const char* drills = std::getenv("NOISE_ORDNANCE_DRILLS_DIR");
  if (drills == nullptr || drills[0] == '\0') {
    std::printf("FAIL bypass-alignment: NOISE_ORDNANCE_DRILLS_DIR not set\n");
    return 1;
  }
  std::vector<float> clean;
  if (!readWavMono16((std::string(drills) + "/paint-drying-clean.wav").c_str(),
                     clean)) {
    std::printf("FAIL bypass-alignment: could not read clean drill WAV\n");
    return 1;
  }
  // 12 s of speech: bypass 3-6 s, final window 10-11 s is steady
  // (4+ s after un-bypass, far beyond the ~117 ms pipeline refill).
  const size_t n = 12 * 48000;
  if (clean.size() < n) {
    std::printf("FAIL bypass-alignment: clean WAV too short (%zu)\n", clean.size());
    return 1;
  }
  const std::vector<float> in(clean.begin(), clean.begin() + n);

  DenoiserEngine e;
  if (!e.init(48000.0, md, 0) || !e.modelLoaded()) {
    std::printf("FAIL bypass-alignment: model did not load\n");
    return 1;
  }
  e.setReduction(1.0f);
  e.startWorker();

  // Bypass ON at block 1125 (3 s), OFF at block 2250 (6 s).
  // 1125 blocks is far past the ~112 ms mix settle, so the old
  // bypass-settled skip engaged for nearly the whole bypass.
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
      // caught up so the test measures the bypass gap, not wet
      // starvation shift.
      if (!e.waitWorkerIdle(5000)) {
        drainFailed.store(true, std::memory_order_release);
        break;
      }
    }
    audioDone.store(true, std::memory_order_release);
  });
  std::thread ui([&] {
    bool on = false, off = false;
    while (!off) {
      const size_t done = blocksDone.load(std::memory_order_acquire);
      if (!on && (done >= 1125 || audioDone.load(std::memory_order_acquire))) {
        e.setBypassed(true);
        on = true;
      } else if (on && (done >= 2250 || audioDone.load(std::memory_order_acquire))) {
        e.setBypassed(false);
        off = true;
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  });
  audio.join();
  ui.join();
  e.stopWorker();

  if (drainFailed.load()) {
    std::printf("FAIL bypass-alignment: worker did not drain during run\n");
    return 1;
  }
  const uint64_t frames = e.workerFrames();
  // Bypass window (4-5 s, mix settled at 0): output must be delayed
  // dry, i.e. the clean input at exactly the model latency.
  const float bypassCorr = corrAtLag(out, in, 4 * 48000, 1 * 48000, 5616);
  // Final steady window (10-11 s): wet must be back at exactly the
  // model latency, not shifted early by the bypass gap.
  const float finalCorr = corrAtLag(out, in, 10 * 48000, 1 * 48000, 5616);
  if (frames > 100 && allFinite(out) && bypassCorr > 0.9f && finalCorr > 0.9f) {
    std::printf("PASS bypass-alignment: 3 s bypass under audio, bypass "
                "window corr %.3f, final window corr %.3f at lag 5616, "
                "worker frames %llu\n",
                bypassCorr, finalCorr, (unsigned long long)frames);
    return 0;
  }
  std::printf("FAIL bypass-alignment: bypass corr %.3f (want > 0.9), "
              "final corr %.3f (want > 0.9 at 5616), worker frames %llu, "
              "finite %d\n",
              bypassCorr, finalCorr, (unsigned long long)frames,
              (int)allFinite(out));
  return 1;
}
