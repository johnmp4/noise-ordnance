// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
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
  // Order matches the plugin's Model enumeration parameter.
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
  void clearPipeline();

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
