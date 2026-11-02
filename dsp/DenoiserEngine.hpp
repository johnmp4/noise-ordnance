// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "df.h"
#include "speex/speex_resampler.h"
}

class RingBuffer;

// DeepFilterNet engine: the same shell as the RNNoise engine (hum notch,
// resample-to-48k, wet/dry mix, delta monitor, delay-compensated dry),
// with the model core replaced by libDF (Rust, via its C API -- see
// dsp/df.h). No Rust code lives in this repo; the engine only calls
// df_create / df_process_frame / df_set_* / df_free.
//
// Model files are NOT in git: they live in third_party/models/ (see
// third_party/download-models.sh) or in the plugin bundle's models/
// directory, and are loaded from their .tar.gz paths at runtime.
//
// Output assembly follows the lessons of the Oct 2026 block-invariance
// fix: wet rendering is gated on a fixed absolute output position
// (wetStart_), starved samples fall back to delayed dry (never
// undelayed dry), the wet buffer is topped up eagerly on every call,
// and consumption subtracts only what was actually popped. That is what
// makes output bit-identical across host block sizes.
class DenoiserEngine {
public:
  // Model selection: index into the model file table (see .cpp).
  // The plugin exposes this as a boolean Low Latency Model switch
  // (OFF = kModelDF3, ON = kModelDF3LL); the harness and unit tests
  // address models by index directly.
  // NOTE: DeepFilterNet2 is deliberately NOT offered. Both DF2 models
  // (full and _ll) panic inside tract's reshape lowering at load time
  // (df_create aborts the host process) under libDF 0.5.6/tract 0.19.16
  // and main/tract 0.21.4, verified with the repo's own deep-filter
  // binary as well as this engine. A model that aborts the host cannot
  // be a dropdown entry. See BRANCH-NOTES.md.
  static constexpr int kModelDF3 = 0;
  static constexpr int kModelDF3LL = 1;
  static constexpr int kModelCount = 2;
  static const char* modelFileName(int index);
  static const char* modelDisplayName(int index);

  DenoiserEngine();
  ~DenoiserEngine();

  // Load the model at modelDir/modelFileName(modelIndex) and prepare the
  // pipeline at hostSampleRate. Returns false if the model file is
  // missing/unreadable -- the engine then runs dry passthrough (no
  // crash: df_create panics on a bad path, so it is never called
  // without a stat-checked file). latencySamples() still reports the
  // table latency in that case, so host delay compensation stays
  // aligned; modelLoaded() reports whether denoising is actually active.
  bool init(double hostSampleRate, const std::string& modelDir, int modelIndex);
  // Full state clear, including the DF state's internal history
  // (reloads the current model). init() does not call reset(); it builds
  // the pipeline directly so the model is loaded exactly once.
  void reset();
  void setReduction(float v);
  void setPostFilterBeta(float beta);  // 0 = off, 0..0.05, default 0.02
  void setAttenLim(float db);          // 0..100 dB, default 100 = no limit
  void setDeltaMonitor(bool enable);   // true = output removed signal
  void setInputTrim(float db);
  void setOutputTrim(float db);
  void setBypassed(bool b);
  void setHum(bool enabled, float mainsFreq);
  // Synchronous model reload (test branch: load time is not a concern).
  // If the engine was never init()'d, just records the index for the
  // next init(). If the file is missing, keeps the current model and
  // returns false. On success, latencySamples() reflects the new model
  // and the pipeline is cleared (brief dry moment at the switch).
  bool setModel(int index);
  int modelIndex() const { return modelIndex_; }

  // Silence gate (CPU saver, Oct 31, 2026): a DF frame whose RMS sits
  // below kSilentGateRms is not worth a full inference -- the model
  // outputs ~zero on such frames anyway (verified: the silence unit
  // test, and the impulse probe that gave no measurable output). But
  // gating must not truncate speech tails: the model's output lags its
  // input by a few frames (lookahead + STFT), so the first
  // kSilentGateDelayFrames silent frames are still processed normally
  // (their outputs carry the tail), and only subsequent silent frames
  // are skipped (zeros pushed, so output timing never shifts). A
  // non-silent frame resets the count and resumes inference at once.
  // The gate decision depends only on frame content, never on host
  // block size, so block invariance is unaffected. Savings depend on
  // the material: large for speech with pauses or idle inputs, ~zero
  // for continuous noise (whose RMS never drops below the threshold).
  static constexpr float kSilentGateRms = 0.001f;  // -60 dBFS RMS
  static constexpr uint32_t kSilentGateDelayFrames = 8;  // 80 ms @48k

  // Frames skipped by the silence gate (test observability; the gate
  // is content-driven, so a test asserting it engaged needs a count).
  uint64_t gatedFrames() const { return gatedFrames_; }

  // Async inference (worker thread, Oct 31, 2026): the fix for the
  // 128-sample deadline problem. DF inference for one 480-sample frame
  // costs more than a 128-sample block's 2.7 ms budget, so running it
  // on the audio thread xruns periodically even though average CPU is
  // only ~25%. The worker runs the same frames in the same order; the
  // audio thread only resamples, shuffles rings, and renders. This
  // adds NO reported latency: the worker's time budget is the existing
  // wetStart slack (4096 samples = 85 ms at 48 kHz) -- a frame's input
  // arrives ~117 ms before its render position, so the worker has to
  // keep up on average, not per block. If it ever falls behind, the
  // render loop falls back to delayed dry (a brief un-denoised
  // moment), never a dropout or a crash. start/stop are NOT
  // thread-safe against each other; in practice the host serializes
  // activate/deactivate/setModel/reset/destruction.
  void startWorker();
  void stopWorker();
  // Test/diagnostic use only, never the audio thread: block until the
  // worker has consumed every input frame (or the timeout elapses).
  bool waitWorkerIdle(int timeoutMs = 5000);
  uint64_t workerFrames() const { return workerFrames_.load(); }  // frames handled by the worker (including gate-skipped ones)

  void process(const float* in, float* out, uint32_t frames);

  uint32_t latencySamples() const;
  bool modelLoaded() const { return dfState_ != nullptr; }

private:
  static constexpr uint32_t kModelRate = 48000;
  static constexpr uint32_t kScratchSamples = 8192;
  // Wet-render gate, in 48 kHz samples, scaled by hostRate/48000.
  // Rendering starts consuming wet samples at this absolute output
  // position. It must exceed the chain's input lookahead (DF lookahead
  // + resampler delays, ~2000 samples at 48 kHz worst case) so that a
  // wet sample is always available when its render position arrives,
  // regardless of host block size -- that is what makes output
  // block-size invariant. If block invariance ever breaks, bump this.
  static constexpr uint32_t kWetStart48k = 4096;

  static void debugLog(const char* fmt, ...);
  bool loadModelFile(const std::string& path);
  // Create + validate a DF state WITHOUT touching member state (the
  // slow df_create lives here). setModel/reset load through this, then
  // swap under the exclusive lock; loadModelFile uses it too.
  DFState* createModelState(const std::string& path, size_t& outFrameLen);
  // Caller must hold stateMutex_ exclusive, except init(), which runs
  // single-threaded before activation and never races process().
  void clearPipeline();
  // Run one DF frame from the input ring to the output ring, applying
  // the silence gate (see process()). A bypass-settled skip used to
  // live here too; it was removed Nov 1, 2026 (bypass echo fix) --
  // skipping inference across a bypass gap corrupts DF's state.
  // Callers must guarantee inRing_->available() >= frameLen_ and
  // dfState_ != nullptr, and must hold stateMutex_ shared. Runs on
  // the audio thread in sync mode and on the worker thread in async
  // mode -- never both at once (the worker runs only while the plugin
  // has started it; tests use sync).
  void processDfFrame();
  // process() body without locking (process() takes shared and then
  // calls this per chunk; shared_mutex is not recursive, so the
  // large-block split must not re-enter process()).
  void processInternal(const float* in, float* out, uint32_t frames);

  double hostRate_ = 48000.0;
  float reduction_ = 1.0f;
  float postFilterBeta_ = 0.02f;  // 0.0 = post-filter off
  float attenLimDb_ = 100.0f;     // 100 = no attenuation limit
  bool deltaMonitor_ = false;     // true = output dry-wet (removed)

  float inTrim_ = 1.0f;
  float outTrim_ = 1.0f;
  float mix_ = 0.0f;
  float mixTarget_ = 1.0f;
  bool bypassed_ = false;
  bool initialized_ = false;

  std::string modelDir_;
  int modelIndex_ = kModelDF3;
  uint32_t latency48k_ = 0;  // measured, per model (see .cpp table)
  uint32_t dryDelayLen_ = 0;
  uint32_t wetStart_ = 0;
  uint64_t streamPos_ = 0;

  // Dry-path delay line (compensates wet-path latency so dry/wet mix aligns)
  std::vector<float> dryDelay_;
  uint32_t dryDelayWrite_ = 0;

  bool humEnabled_ = false;
  float humMains_ = 60.0f;
  // Hum notch cascade (module 1): one biquad per f0..5f0 at host rate.
  // Applied to the whole input BEFORE the dry/wet split, so the delay-line
  // dry and the denoised wet are both hum-free at any mix position.
  // Bypassed = true passthrough: the notch is off when the effect is off.
  static constexpr uint32_t kHumSections = 5;  // f0 + 4 harmonics
  static constexpr float kHumQ = 25.0f;        // ~2.4 Hz wide at 60 Hz
  float humB0_[kHumSections] = {0.0f};
  float humB1_[kHumSections] = {0.0f};
  float humB2_[kHumSections] = {0.0f};
  float humA1_[kHumSections] = {0.0f};
  float humA2_[kHumSections] = {0.0f};
  float humS1_[kHumSections] = {0.0f};
  float humS2_[kHumSections] = {0.0f};
  uint32_t humSections_ = 0;
  float humScratch_[kScratchSamples] = {0.0f};
  void rebuildHumFilters();

  DFState* dfState_ = nullptr;
  uint32_t frameLen_ = 0;
  std::vector<float> frameBuf_;
  std::vector<float> frameOutBuf_;
  // Silence-gate state (see the constants above): consecutive silent
  // frames seen, and frames actually skipped. Reset in clearPipeline
  // (fresh stream); the gate decision itself lives in processDfFrame.
  // Owned by whichever thread runs DF (audio in sync mode, worker in
  // async mode -- never both, since mode switches stop the worker).
  uint32_t silentFrames_ = 0;
  uint64_t gatedFrames_ = 0;
  // Thread-safety protocol (model-switch race fix, Nov 1, 2026).
  // setModel/reset swap dfState_ and reset the rings, resamplers, dry
  // delay line, and stream position -- all state the audio thread
  // (process, including its sync fallback) and the worker thread
  // (processDfFrame) touch. Stopping the worker is NOT enough: while
  // it is down, the audio thread falls back to synchronous DF, so it
  // can be inside df_process_frame while the UI thread frees
  // dfState_ -- proven by a multithreaded probe that crashed in
  // tract on freed graph memory (index out of bounds on a garbage
  // index). Protocol: process() takes try_lock_shared -- it NEVER
  // blocks the audio thread; if a switch holds exclusive, that block
  // passes audio through untouched (the post-switch reset realigns
  // everything, so the skipped block leaves no offset behind).
  // workerMain() takes lock_shared across each frame (param
  // application + processDfFrame). setModel()/reset() do the slow
  // model load OUTSIDE the lock (the audio thread keeps running the
  // old model on valid, untouched state), then take lock()
  // (exclusive) only for the fast swap + clearPipeline. The plain
  // float params (reduction, trims, bypass, hum switches) stay
  // lock-free: a stale read costs at most one block, inaudible.
  // The df_set_* setters' direct-apply path (worker down) takes
  // shared too -- a setter must never touch dfState_ while the audio
  // thread is inferring on it. The worker-loop wait predicate reads
  // dfState_/frameLen_ without the lock, but only as a wakeup hint;
  // every actual use is re-checked under shared.
  std::shared_mutex stateMutex_;
  // Worker thread state. dfState_ is touched only by the worker while
  // it runs, so the df_set_* setters defer through atomics when the
  // worker is up (applied at the top of the worker loop) and apply
  // directly when it is down (sync mode, activate-time re-apply).
  // bypassed_/mix_/deltaMonitor_ are plain floats read by the worker
  // for the skip decision; a stale read costs at most one frame's
  // skip/no-skip, inaudible under the 10 ms mix ramp.
  std::thread worker_;
  std::mutex workerMutex_;
  std::condition_variable workerCv_;
  std::atomic<bool> workerRunning_{false};
  std::atomic<float> pendingBeta_{0.02f};
  std::atomic<float> pendingAtten_{100.0f};
  std::atomic<bool> betaDirty_{false};
  std::atomic<bool> attenDirty_{false};
  std::atomic<uint64_t> workerFrames_{0};
  void workerMain();
  SpeexResamplerState* resIn_ = nullptr;
  SpeexResamplerState* resOut_ = nullptr;
  RingBuffer* inRing_ = nullptr;
  RingBuffer* outRing_ = nullptr;
  float inScratch_[kScratchSamples] = {0.0f};
  float wetScratch_[kScratchSamples] = {0.0f};
  float pendingModel_[kScratchSamples] = {0.0f};
  float pendingHost_[kScratchSamples] = {0.0f};
  uint32_t pendingModelCount_ = 0;
  uint32_t pendingHostCount_ = 0;
};
