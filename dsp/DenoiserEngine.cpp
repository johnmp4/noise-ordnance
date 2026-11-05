// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DenoiserEngine.hpp"

#include "RingBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

// M_PI is not standard C++: strict -std=c++17 hides it on MinGW-w64,
// while glibc still exposes it. Define it when missing.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void DenoiserEngine::debugLog(const char* fmt, ...) {
  (void)fmt;
#ifdef DEBUG
  FILE* f = fopen("/tmp/no-debug.log", "a");
  if (!f) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fclose(f);
#endif
}

namespace {

// Model files (in the model directory) and display names, indexed by
// DenoiserEngine::kModelDF3 / kModelDF3LL. The plugin's Low Latency
// Model switch maps OFF -> index 0, ON -> index 1. DeepFilterNet2 is not
// here: it does not load (see DenoiserEngine.hpp).
const char* kModelFiles[DenoiserEngine::kModelCount] = {
    "DeepFilterNet3_onnx.tar.gz",
    "DeepFilterNet3_ll_onnx.tar.gz",
};
const char* kModelNames[DenoiserEngine::kModelCount] = {
    "DeepFilterNet3",
    "DeepFilterNet3 Low Latency",
};

// Measured wet-path latency at 48 kHz, per model, via cross-correlation
// (tests/check-latency.py): the clean drill speech is run through the
// engine at 100% wet and a mid-file segment of input vs output is
// correlated; the peak lag is the number below. An impulse probe does
// NOT work reliably for DF: the model gates near-silent frames, so a
// lone impulse in silence gives no measurable output under libDF 0.5.6
// (and only a marginal 1.6e-4 peak under main). latencySamples() scales these by hostRate/48000; the
// samplerate unit test verifies the scaling holds within 10% at
// 44.1/96 kHz.
// Re-measure if the model files change: run tests/check-latency.py
// and put the measured numbers here. Do NOT guess these numbers.
//   DeepFilterNet3: 5616 = wetStart 4096 + chain lag 1520 (model STFT +
//     lookahead + resamplers). At 48 kHz that is 117 ms total.
//   DeepFilterNet3 Low Latency: 4656 = 4096 + 560 (97 ms total).
// DeepFilterNet2 is not in this table: it does not load (see
// DenoiserEngine.hpp), so it has no meaningful latency here.
const uint32_t kModelLatency48k[DenoiserEngine::kModelCount] = {
    5616,  // DeepFilterNet3 -- measured by speech cross-correlation (tests/check-latency.py)
    4656,  // DeepFilterNet3 Low Latency -- measured by speech cross-correlation
};

bool fileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

const char* DenoiserEngine::modelFileName(int index) {
  if (index < 0 || index >= kModelCount)
    return kModelFiles[kModelDF3];
  return kModelFiles[index];
}

const char* DenoiserEngine::modelDisplayName(int index) {
  if (index < 0 || index >= kModelCount)
    return kModelNames[kModelDF3];
  return kModelNames[index];
}

DenoiserEngine::DenoiserEngine() = default;

DenoiserEngine::~DenoiserEngine() {
  // Stop the worker FIRST: it touches the rings and dfState_ below.
  stopWorker();
  delete inRing_;
  delete outRing_;

  if (resIn_ != nullptr) {
    speex_resampler_destroy(resIn_);
    resIn_ = nullptr;
  }
  if (resOut_ != nullptr) {
    speex_resampler_destroy(resOut_);
    resOut_ = nullptr;
  }
  if (dfState_ != nullptr) {
    df_free(dfState_);
    dfState_ = nullptr;
  }
}

// Load a model file into dfState_. The file MUST exist: df_create panics
// (aborts) on a bad path, so a stat check happens before every call.
// On failure the previous state is preserved (or stays null on first
// load) and false is returned. On success the current post-filter beta
// and attenuation limit are applied (df_create starts with the
// post-filter off).
DFState* DenoiserEngine::createModelState(const std::string& path, size_t& outFrameLen) {
  if (!fileExists(path))
    return nullptr;
  DFState* next = df_create(path.c_str(), attenLimDb_, nullptr);
  if (next == nullptr)
    return nullptr;
  const size_t fl = df_get_frame_length(next);
  if (fl == 0 || fl > 4096) {
    df_free(next);
    return nullptr;
  }
  outFrameLen = fl;
  return next;
}

bool DenoiserEngine::loadModelFile(const std::string& path) {
  size_t fl = 0;
  DFState* next = createModelState(path, fl);
  if (next == nullptr)
    return false;
  if (dfState_ != nullptr)
    df_free(dfState_);
  dfState_ = next;
  df_set_post_filter_beta(dfState_, postFilterBeta_);
  df_set_atten_lim(dfState_, attenLimDb_);
  frameLen_ = static_cast<uint32_t>(fl);
  frameBuf_.assign(frameLen_, 0.0f);
  frameOutBuf_.assign(frameLen_, 0.0f);
  return true;
}

void DenoiserEngine::clearPipeline() {
  if (inRing_ != nullptr)
    inRing_->reset();
  if (outRing_ != nullptr)
    outRing_->reset();

  if (resIn_ != nullptr)
    speex_resampler_reset_mem(resIn_);
  if (resOut_ != nullptr)
    speex_resampler_reset_mem(resOut_);

  dryDelay_.assign(dryDelayLen_ + 8192, 0.0f);
  dryDelayWrite_ = 0;
  mix_ = 0.0f;
  mixTarget_ = reduction_;
  streamPos_ = 0;
  wetPos_ = wetStart_;  // front wet belongs to the first wet position
  pendingModelCount_ = 0;
  pendingHostCount_ = 0;
  silentFrames_ = 0;
  gatedFrames_ = 0;
  std::fill(frameBuf_.begin(), frameBuf_.end(), 0.0f);
  std::fill(frameOutBuf_.begin(), frameOutBuf_.end(), 0.0f);
  std::memset(inScratch_, 0, sizeof(inScratch_));
  std::memset(wetScratch_, 0, sizeof(wetScratch_));
  std::memset(pendingModel_, 0, sizeof(pendingModel_));
  std::memset(pendingHost_, 0, sizeof(pendingHost_));
  std::memset(humS1_, 0, sizeof(humS1_));
  std::memset(humS2_, 0, sizeof(humS2_));
}

bool DenoiserEngine::init(double hostSampleRate, const std::string& modelDir, int modelIndex) {
  // The worker (if running) touches the rings rebuilt below; stop it.
  // Callers that want async (the plugin) restart it after init.
  stopWorker();
  hostRate_ = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
  modelDir_ = modelDir;
  if (modelIndex >= 0 && modelIndex < kModelCount)
    modelIndex_ = modelIndex;
  rebuildHumFilters();  // coefficients depend on host rate

  delete inRing_;
  delete outRing_;
  inRing_ = new RingBuffer(16384);
  outRing_ = new RingBuffer(16384);

  if (resIn_ != nullptr)
    speex_resampler_destroy(resIn_);
  if (resOut_ != nullptr)
    speex_resampler_destroy(resOut_);
  int err = 0;
  resIn_ = speex_resampler_init(1, static_cast<spx_uint32_t>(hostRate_), kModelRate, 5, &err);
  resOut_ = speex_resampler_init(1, kModelRate, static_cast<spx_uint32_t>(hostRate_), 5, &err);

  // Pipeline delay from the measured per-model table. This is the value
  // latencySamples() reports, so the host's delay compensation matches
  // the observed wet-path peak (verified by the impulse unit test).
  latency48k_ = kModelLatency48k[modelIndex_];
  dryDelayLen_ = static_cast<uint32_t>(std::round(latency48k_ * hostRate_ / 48000.0));
  wetStart_ = static_cast<uint32_t>(std::round(kWetStart48k * hostRate_ / 48000.0));

  bool ok = false;
  if (!modelDir_.empty())
    ok = loadModelFile(modelDir_ + "/" + modelFileName(modelIndex_));
  initialized_ = true;
  clearPipeline();
  return ok;
}

void DenoiserEngine::reset() {
  // Reload the current model to clear the DF state's internal history
  // (STFT + lookahead buffers), then clear the pipeline. A failed reload
  // keeps the previous state. The worker stops across the reload and
  // restarts if it was up; the swap + clearPipeline run under the
  // exclusive lock so neither the audio thread nor the worker can be
  // touching the swapped/reset state (see stateMutex_ protocol).
  const bool wasRunning = workerRunning_.load();
  stopWorker();
  DFState* next = nullptr;
  size_t fl = 0;
  if (initialized_ && !modelDir_.empty())
    next = createModelState(modelDir_ + "/" + modelFileName(modelIndex_), fl);
  stateMutex_.lock();
  if (next != nullptr) {
    if (dfState_ != nullptr)
      df_free(dfState_);
    dfState_ = next;
    df_set_post_filter_beta(dfState_, postFilterBeta_);
    df_set_atten_lim(dfState_, attenLimDb_);
    frameLen_ = static_cast<uint32_t>(fl);
    frameBuf_.assign(frameLen_, 0.0f);
    frameOutBuf_.assign(frameLen_, 0.0f);
  }
  clearPipeline();
  stateMutex_.unlock();
  if (wasRunning)
    startWorker();
}

bool DenoiserEngine::setModel(int index) {
  if (index < 0 || index >= kModelCount)
    return false;
  if (!initialized_) {
    // Not init()'d yet (plugin constructed but not activated): just
    // record the selection; activate() loads it.
    modelIndex_ = index;
    return true;
  }
  if (modelDir_.empty())
    return false;
  // The worker touches dfState_: stop it across the load, and restart
  // it if it was up -- including on load failure, where the previous
  // state is preserved. The slow load runs OUTSIDE the lock: the audio
  // thread keeps running the old model (sync fallback) on valid,
  // untouched state. Only the fast swap + clearPipeline run under the
  // exclusive lock (see stateMutex_ protocol): stopping the worker
  // alone does NOT make the swap safe, because the audio thread's
  // sync fallback can be inside df_process_frame while the UI thread
  // frees dfState_ -- that race crashed a probe in tract.
  const bool wasRunning = workerRunning_.load();
  stopWorker();
  size_t fl = 0;
  DFState* next = createModelState(modelDir_ + "/" + modelFileName(index), fl);
  if (next == nullptr) {
    if (wasRunning)
      startWorker();
    return false;  // keeps the current model
  }
  stateMutex_.lock();
  if (dfState_ != nullptr)
    df_free(dfState_);
  dfState_ = next;
  df_set_post_filter_beta(dfState_, postFilterBeta_);
  df_set_atten_lim(dfState_, attenLimDb_);
  frameLen_ = static_cast<uint32_t>(fl);
  frameBuf_.assign(frameLen_, 0.0f);
  frameOutBuf_.assign(frameLen_, 0.0f);
  modelIndex_ = index;
  latency48k_ = kModelLatency48k[index];
  dryDelayLen_ = static_cast<uint32_t>(std::round(latency48k_ * hostRate_ / 48000.0));
  clearPipeline();
  stateMutex_.unlock();
  if (wasRunning)
    startWorker();
  return true;
}

// One DF frame: pop a frame from the input ring, run it, push exactly
// one frame's worth of output to the output ring.
//   NOTE (Nov 1, 2026): this used to skip inference entirely when
// bypass had settled (bypassed_ && mix_ == 0 && !deltaMonitor_),
// on the theory that wet multiplied by zero is pure waste and the
// stream would restart cleanly because the model's delay is
// lookahead + STFT, not wall-clock time. That theory was WRONG:
// the gap corrupts DF's internal temporal state, and on un-bypass
// the wet stream resumes ~70 ms early and stays shifted until the
// next pipeline reset (a model switch) -- the live bypass echo,
// reproduced in a stereo probe (see tests/unit/test-bypass-alignment.cpp).
// So inference now runs unconditionally here; during bypass the
// render loop discards the wet (mix_ == 0 outputs delayed dry),
// which costs active CPU while bypassed but keeps DF's state
// continuous. The silence gate below is different: gated frames
// push zeros (not nothing) so the wet stream's frame count -- and
// hence wet/dry alignment -- never shifts.
//   Silence gate: see the constants in the header.
void DenoiserEngine::processDfFrame() {
  const uint32_t popped = inRing_->pop(frameBuf_.data(), frameLen_);
  if (popped != frameLen_)
    return;  // caller guarantees availability; be safe, not sorry
  double sumSq = 0.0;
  for (uint32_t i = 0; i < frameLen_; ++i) {
    const double x = frameBuf_[i];
    sumSq += x * x;
  }
  const float rms = static_cast<float>(std::sqrt(sumSq / frameLen_));
  if (rms < kSilentGateRms && silentFrames_ >= kSilentGateDelayFrames) {
    std::fill(frameOutBuf_.begin(), frameOutBuf_.end(), 0.0f);
    outRing_->push(frameOutBuf_.data(), frameLen_);
    ++gatedFrames_;
    return;
  }
  df_process_frame(dfState_, frameBuf_.data(), frameOutBuf_.data());
  outRing_->push(frameOutBuf_.data(), frameLen_);
  if (rms < kSilentGateRms)
    ++silentFrames_;
  else
    silentFrames_ = 0;
}

void DenoiserEngine::setPostFilterBeta(float beta) {
  postFilterBeta_ = beta;
  if (workerRunning_.load()) {
    // The worker owns dfState_ while it runs: defer, never touch it
    // from here (a setter from automation must never race inference).
    // The worker applies pending values at the top of its loop.
    pendingBeta_.store(beta);
    betaDirty_.store(true);
    return;
  }
  if (dfState_ != nullptr) {
    // Direct apply only happens with the worker down -- but the audio
    // thread's sync fallback may still be inferring, so take shared.
    stateMutex_.lock_shared();
    if (dfState_ != nullptr)
      df_set_post_filter_beta(dfState_, beta);
    stateMutex_.unlock_shared();
  }
}

void DenoiserEngine::setAttenLim(float db) {
  attenLimDb_ = db;
  if (workerRunning_.load()) {
    pendingAtten_.store(db);
    attenDirty_.store(true);
    return;
  }
  if (dfState_ != nullptr) {
    // See setPostFilterBeta: shared, because the audio thread's sync
    // fallback may be inferring on dfState_ right now.
    stateMutex_.lock_shared();
    if (dfState_ != nullptr)
      df_set_atten_lim(dfState_, db);
    stateMutex_.unlock_shared();
  }
}

void DenoiserEngine::setDeltaMonitor(bool enable) {
  deltaMonitor_ = enable;
}

void DenoiserEngine::setReduction(float v) {
  reduction_ = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  mixTarget_ = reduction_;
}

void DenoiserEngine::setInputTrim(float db) {
  inTrim_ = std::pow(10.0f, db / 20.0f);
}

void DenoiserEngine::setOutputTrim(float db) {
  outTrim_ = std::pow(10.0f, db / 20.0f);
}

void DenoiserEngine::setBypassed(bool b) {
  bypassed_ = b;
}

void DenoiserEngine::setHum(bool enabled, float mainsFreq) {
  humEnabled_ = enabled;
  humMains_ = mainsFreq == 50.0f ? 50.0f : 60.0f;
  rebuildHumFilters();
}

// Designs the hum notch cascade for the current host rate: narrow RBJ
// cookbook notches at f0..5f0 (e.g. 60/120/180/240/300 Hz). Q=25 keeps each
// notch a few Hz wide -- deep on the hum, inaudible on speech.
void DenoiserEngine::rebuildHumFilters() {
  humSections_ = 0;
  std::memset(humS1_, 0, sizeof(humS1_));
  std::memset(humS2_, 0, sizeof(humS2_));
  if (!humEnabled_)
    return;
  for (uint32_t k = 1; k <= kHumSections; ++k) {
    const double f = static_cast<double>(humMains_) * k;
    if (f >= hostRate_ * 0.5)
      break;  // stay below Nyquist at low host rates
    const double w0 = 2.0 * M_PI * f / hostRate_;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kHumQ);
    const double a0 = 1.0 + alpha;
    const uint32_t s = humSections_;
    humB0_[s] = static_cast<float>(1.0 / a0);
    humB1_[s] = static_cast<float>(-2.0 * cosw / a0);
    humB2_[s] = static_cast<float>(1.0 / a0);
    humA1_[s] = static_cast<float>(-2.0 * cosw / a0);
    humA2_[s] = static_cast<float>((1.0 - alpha) / a0);
    ++humSections_;
  }
}

void DenoiserEngine::process(const float* in, float* out, uint32_t frames) {
  if (in == nullptr || out == nullptr || frames == 0)
    return;
  // try_lock_shared: never block the audio thread. If a model switch
  // holds the exclusive lock, this block passes audio through
  // untouched; the post-switch pipeline reset realigns everything, so
  // the skipped block leaves no offset behind. The large-block split
  // runs under the single shared hold (processInternal per chunk, not
  // process -- shared_mutex is not recursive).
  if (!stateMutex_.try_lock_shared()) {
    for (uint32_t i = 0; i < frames; ++i)
      out[i] = in[i];
    return;
  }
  // Very large blocks would overflow the fixed scratch buffers: split
  // them into scratch-sized chunks (identical to the host calling with
  // smaller blocks, which this engine is invariant to -- see the
  // block-invariance unit test).
  if (frames > kScratchSamples) {
    uint32_t done = 0;
    while (done < frames) {
      const uint32_t n = std::min(kScratchSamples, frames - done);
      processInternal(in + done, out + done, n);
      done += n;
    }
  } else {
    processInternal(in, out, frames);
  }
  stateMutex_.unlock_shared();
}

// Stage wet for rendering: pop available model-rate wet from outRing_
// into pendingModel_, then resample into pendingHost_. Popping into
// pendingModel_ (not straight into the resampler) keeps the
// resampler's input stream contiguous across blocks.
void DenoiserEngine::stageWet() {
  {
    const uint32_t room = kScratchSamples - pendingModelCount_;
    uint32_t take = static_cast<uint32_t>(outRing_->available());
    if (take > room)
      take = room;
    if (take > 0) {
      outRing_->pop(pendingModel_ + pendingModelCount_, take);
      pendingModelCount_ += take;
    }
    // Popping freed output-ring room the worker may be waiting on.
    if (workerRunning_.load())
      workerCv_.notify_one();
  }

  // Eager resample top-up: run the output resampler whenever there are
  // model samples waiting and host-side room, appending into
  // pendingHost_. The buffer stays near full instead of sawtoothing to
  // zero, so the render loop below (almost) never starves -- and when
  // it does, the fallback is delayed dry, not undelayed dry. The
  // resampler's output stream depends only on its input stream, not on
  // chunking, so this stays block-size invariant.
  if (pendingModelCount_ > 0 && pendingHostCount_ < kScratchSamples) {
    spx_uint32_t modelIn = pendingModelCount_;
    spx_uint32_t hostOut = kScratchSamples - pendingHostCount_;
    speex_resampler_process_float(resOut_, 0, pendingModel_, &modelIn,
                                  pendingHost_ + pendingHostCount_, &hostOut);
    pendingHostCount_ += hostOut;
    const uint32_t usedModel = modelIn;
    if (usedModel < pendingModelCount_) {
      std::memmove(pendingModel_, pendingModel_ + usedModel,
                   (pendingModelCount_ - usedModel) * sizeof(float));
    }
    pendingModelCount_ -= usedModel;
  }
}

// Discard stale wet (offline-render fix, Nov 2, 2026): pendingHost_[0]
// belongs to position wetPos_; if the render has advanced past it
// (starvation gaps fell back to dry), those samples are for positions
// already rendered and must never sound. Drop the stale prefix so the
// front aligns with streamPos_ (or as close as the buffer allows --
// if the buffer empties first, the remaining gap persists and the
// next staged wet gets dropped the same way on arrival).
void DenoiserEngine::dropStaleWet() {
  if (streamPos_ <= wetPos_)
    return;
  const uint64_t stale = streamPos_ - wetPos_;
  const uint32_t drop =
      stale > pendingHostCount_ ? pendingHostCount_ : static_cast<uint32_t>(stale);
  if (drop == 0)
    return;
  if (drop < pendingHostCount_) {
    std::memmove(pendingHost_, pendingHost_ + drop,
                 (pendingHostCount_ - drop) * sizeof(float));
  }
  pendingHostCount_ -= drop;
  wetPos_ += drop;
}

void DenoiserEngine::processInternal(const float* in, float* out, uint32_t frames) {
  if (!initialized_) {
    // Never init()'d (e.g. plugin constructed but not activated):
    // true passthrough. No model load is attempted from the audio
    // thread -- and, importantly, not during TTL generation either,
    // which instantiates the plugin without activating it.
    for (uint32_t i = 0; i < frames; ++i)
      out[i] = in[i];
    return;
  }

  std::fill(inScratch_, inScratch_ + kScratchSamples, 0.0f);
  std::fill(wetScratch_, wetScratch_ + kScratchSamples, 0.0f);

  // Module 1: hum notch. Host rate, whole input, before the dry/wet split:
  // both the delay-line dry and the denoised wet are hum-free, so middle
  // mix positions stay hum-free too. Bypassed = true passthrough: the
  // notch is off when the effect is off.
  const float* sig = in;
  if (humEnabled_ && !bypassed_ && humSections_ > 0) {
    for (uint32_t i = 0; i < frames; ++i) {
      float x = in[i];
      for (uint32_t s = 0; s < humSections_; ++s) {
        // Direct Form II transposed notch.
        const float y = humB0_[s] * x + humS1_[s];
        humS1_[s] = humB1_[s] * x - humA1_[s] * y + humS2_[s];
        humS2_[s] = humB2_[s] * x - humA2_[s] * y;
        x = y;
      }
      humScratch_[i] = x;
    }
    sig = humScratch_;
  }

  // Feed the dry delay line every block (even before wet starts) so it
  // holds real audio history by the time the wet path starts.
  {
    const uint32_t dsize = static_cast<uint32_t>(dryDelay_.size());
    for (uint32_t i = 0; i < frames; ++i) {
      dryDelay_[dryDelayWrite_] = sig[i];
      if (++dryDelayWrite_ >= dsize)
        dryDelayWrite_ = 0;
    }
  }

  for (uint32_t i = 0; i < frames; ++i)
    inScratch_[i] = sig[i] * inTrim_;

  // Resample host rate -> 48 kHz model rate, into the input ring.
  spx_uint32_t inLen = frames;
  spx_uint32_t modelLen = static_cast<spx_uint32_t>(std::ceil(static_cast<double>(frames) * (kModelRate / hostRate_)) + 8.0);
  if (modelLen > kScratchSamples)
    modelLen = kScratchSamples;
  speex_resampler_process_float(resIn_, 0, inScratch_, &inLen, wetScratch_, &modelLen);
  // A failed push (async mode only, worker stalled long enough to fill
  // the 16384-sample input ring) drops this block's model samples:
  // the DF stream gets a gap, and the render loop covers it with
  // delayed dry. Degraded, never corrupt. In sync mode the ring is a
  // bounded sawtooth (0..frameLen_) and the push cannot fail.
  if (modelLen > 0)
    inRing_->push(wetScratch_, modelLen);

  // Run whole DF frames (processDfFrame: the silence gate lives
  // there). DeepFilterNet takes float audio
  // at +-1.0 (unlike RNNoise's int16-scale operating point) -- no
  // scaling here. The DF state buffers lookahead internally, so
  // output frames emerge delayed by the model's latency; that delay
  // is what the measured latency table accounts for.
  // In async mode the worker owns DF execution: the audio thread only
  // wakes it (new input is in the ring above). The worker also wakes
  // on a 5 ms timeout backstop, so a missed notify delays a frame
  // instead of stalling it.
  if (workerRunning_.load()) {
    workerCv_.notify_one();
  } else if (dfState_ != nullptr && frameLen_ > 0) {
    while (inRing_->available() >= frameLen_)
      processDfFrame();
  }

  stageWet();
  dropStaleWet();

  // Bounded wait for worker wet (offline-render fix, Nov 2, 2026):
  // if this block renders post-wetStart_ positions but the staged wet
  // falls short, wait for the worker to produce more (it notifies
  // wetCv_ after every frame) instead of declaring starvation at
  // once. In realtime the worker is ahead, so the first staging
  // already covers the block and this loop never runs -- realtime
  // behavior is unchanged. In a freewheeling export the wait slows
  // rendering to inference speed. On timeout the render below falls
  // back to delayed dry for the shortfall, and the anchor above
  // discards the stale wet when it arrives, so a timeout can cost a
  // brief dry moment but never a smeared (late) wet frame. The loop
  // never runs without a model (wet would never come) or in sync
  // mode (the sync drain above already produced everything).
  if (workerRunning_.load() && dfState_ != nullptr && frameLen_ > 0) {
    const uint64_t blockEnd = streamPos_ + frames;
    const uint64_t firstWet =
        streamPos_ > wetStart_ ? streamPos_ : wetStart_;
    if (blockEnd > firstWet) {
      const uint32_t postWet =
          static_cast<uint32_t>(blockEnd - firstWet);
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(kWetWaitMs);
      while (pendingHostCount_ < postWet) {
        if (!workerRunning_.load() ||
            std::chrono::steady_clock::now() >= deadline)
          break;
        {
          std::unique_lock<std::mutex> lk(wetMutex_);
          wetCv_.wait_for(lk, std::chrono::milliseconds(5));
        }
        stageWet();
        dropStaleWet();
      }
    }
  }

  // Single per-sample render loop, gated on absolute stream position:
  // before wetStart_ (and whenever wet is unavailable) the output is
  // delayed dry; from wetStart_ on, wet samples are consumed strictly
  // in order. Because the gate is absolute, not block-relative, and
  // consumption subtracts only what was actually popped, the output is
  // identical for any host block size.
  const float target = bypassed_ ? 0.0f : mixTarget_;
  const float coeff =
      1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(hostRate_)));

  const uint32_t dsize = static_cast<uint32_t>(dryDelay_.size());
  uint32_t wetUsed = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    // Mix ramp: exponential approach with a stall-detect snap. In float
    // arithmetic the ramp stalls once the increment drops below half an
    // ulp of mix_ (gap ~1.4e-5 at 48 kHz) -- and that gap is above any
    // small fixed snap threshold, so a threshold snap alone leaves a
    // permanent dry leak at steady state. When the update stops moving
    // mix_, jump to the target instead: the step is the stall gap
    // itself (inaudible, <= -85 dB in the mix coefficient).
    const float newMix = mix_ + (target - mix_) * coeff;
    mix_ = (newMix == mix_ && target != mix_) ? target : newMix;
    // Dry sample from dryDelayLen_ ago: input written at
    // (write - frames + i), minus the delay.
    const uint32_t didx =
        (dryDelayWrite_ + 2 * dsize - frames + i - dryDelayLen_) % dsize;
    const float dry = dryDelay_[didx];
    const uint64_t pos = streamPos_ + i;
    float outSample;
    if (pos < wetStart_ || wetUsed >= pendingHostCount_) {
      // Not yet wet, or starved: delayed dry, never undelayed dry --
      // that would shift the sample by the full latency. Dry is never
      // trimmed: output trim is makeup gain for the processed signal,
      // and dry was never processed -- so bypass (which outputs this
      // path once settled) is a true level reference for A/B
      // comparison, and turning the trim never moves the reference.
      outSample = dry;
    } else {
      const float wet = pendingHost_[wetUsed++];
      const float mixed = dry * (1.0f - mix_) + wet * mix_;
      if (deltaMonitor_) {
        // Delta monitor: output what was removed (dry - wet).
        outSample = (dry - mixed) * outTrim_;
      } else {
        // Output trim applies to the wet component only: dry keeps its
        // original level at every mix position, so a gain-matched
        // full-wet output stays level-matched across the whole mix
        // range instead of getting louder toward the middle.
        outSample = dry * (1.0f - mix_) + wet * mix_ * outTrim_;
      }
    }
    out[i] = outSample;
  }
  if (wetUsed > 0) {
    if (wetUsed < pendingHostCount_) {
      std::memmove(pendingHost_, pendingHost_ + wetUsed,
                   (pendingHostCount_ - wetUsed) * sizeof(float));
    }
    pendingHostCount_ -= wetUsed;
    wetPos_ += wetUsed;  // consumed wet advances the anchor with it
  }
  streamPos_ += frames;
}

uint32_t DenoiserEngine::latencySamples() const {
  // The wet-path latency from the measured per-model table, scaled to
  // the host rate. Report it so the host can delay-compensate the
  // plugin's output. The impulse unit test verifies the reported value
  // against the observed wet-path peak on every run.
  return dryDelayLen_;
}

// --- Worker thread (async inference) ---

void DenoiserEngine::startWorker() {
  // Idempotent test-and-set: a second start while running is a no-op.
  // The worker idles (5 ms timeout loop) when no model is loaded; a
  // later setModel restarts it, so a late model load still engages it.
  if (workerRunning_.exchange(true))
    return;
  worker_ = std::thread([this] { workerMain(); });
}

void DenoiserEngine::stopWorker() {
  if (!workerRunning_.exchange(false))
    return;
  workerCv_.notify_all();
  wetCv_.notify_all();  // wake an audio thread in the bounded wait
  if (worker_.joinable())
    worker_.join();
}

bool DenoiserEngine::waitWorkerIdle(int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    // "Idle" means no COMPLETE frame remains for the worker, not an
    // empty ring: the audio thread pushes ~512 samples per block
    // while the worker consumes 480-sample frames, so a partial-frame
    // leftover (< frameLen_) is normal and must not block. (An early
    // version of this waited for available() == 0 and timed out on
    // nearly every block -- 5 s each -- because the leftover almost
    // never hits exactly zero.)
    if (inRing_ == nullptr || frameLen_ == 0 ||
        inRing_->available() < frameLen_)
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void DenoiserEngine::workerMain() {
  while (true) {
    {
      std::unique_lock<std::mutex> lk(workerMutex_);
      // Wait for a runnable frame (input frame available AND output
      // room available), for shutdown, or for the 5 ms backstop. The
      // backstop covers a missed notify: worst case a frame waits an
      // extra 5 ms, far inside the ~117 ms wetStart slack.
      workerCv_.wait_for(lk, std::chrono::milliseconds(5), [&] {
        return !workerRunning_.load() ||
               (dfState_ != nullptr && frameLen_ > 0 && inRing_ != nullptr &&
                outRing_ != nullptr && inRing_->available() >= frameLen_ &&
                outRing_->availableSpace() >= frameLen_);
      });
      if (!workerRunning_.load())
        break;
    }  // release workerMutex_ BEFORE inference -- never hold it across
       // df_process_frame, and never hold it together with
       // stateMutex_ (setModel's stop path must not deadlock).
    // Shared lock across the param application AND the frame: a model
    // switch's exclusive lock waits for the frame to finish, so
    // dfState_ is never freed under a running df_process_frame. The
    // wait predicate above is only a wakeup hint; the checks here,
    // under shared, are authoritative.
    stateMutex_.lock_shared();
    if (!workerRunning_.load()) {
      stateMutex_.unlock_shared();
      break;
    }
    // Apply deferred DF params (see setPostFilterBeta/setAttenLim).
    if (betaDirty_.exchange(false) && dfState_ != nullptr)
      df_set_post_filter_beta(dfState_, pendingBeta_.load());
    if (attenDirty_.exchange(false) && dfState_ != nullptr)
      df_set_atten_lim(dfState_, pendingAtten_.load());
    if (dfState_ == nullptr || frameLen_ == 0 || inRing_ == nullptr ||
        outRing_ == nullptr || inRing_->available() < frameLen_ ||
        outRing_->availableSpace() < frameLen_) {
      stateMutex_.unlock_shared();
      continue;  // spurious wakeup or a race with the audio thread
    }
    processDfFrame();
    ++workerFrames_;
    stateMutex_.unlock_shared();
    // Notify the audio thread's bounded wait (offline-render fix):
    // new wet is in outRing_. The lock is held only for the notify;
    // the audio thread's wait releases wetMutex_ while sleeping, so
    // this can never block behind it.
    {
      std::lock_guard<std::mutex> lk(wetMutex_);
      wetCv_.notify_all();
    }
  }
}
