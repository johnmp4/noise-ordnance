// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include "rnnoise.h"
#include "speex/speex_resampler.h"
}

class RingBuffer;

class DenoiserEngine {
public:
  DenoiserEngine();
  ~DenoiserEngine();

  void init(double hostSampleRate);
  void reset();
  void setReduction(float v);
  void setSmoothing(float s);                  // Gain Smoothing 0.7f * s, Alpha 0.8f * s
  void setBandFloor(float lowDb, float midDb, float highDb); // 3-band floor
  void setVadAdapt(float adapt);               // 0 = off, 1 = full
  void setDeltaMonitor(bool enable);           // true = output removed signal
  void setInputTrim(float db);
  void setOutputTrim(float db);
  void setBypassed(bool b);
  void setHum(bool enabled, float mainsFreq);

  void process(const float* in, float* out, uint32_t frames);

  uint32_t latencySamples() const;

private:
  static constexpr uint32_t kModelRate = 48000;
  static constexpr uint32_t kFrameSize = 480;
  static constexpr uint32_t kScratchSamples = 8192;

  static void debugLog(const char* fmt, ...);

  double hostRate_ = 48000.0;
  float reduction_ = 1.0f;
  float smoothing_ = 0.5f;        // 0.0 = off, 1.0 = full
  float bandFloorLowDb_ = -24.0f;  // 3-band floor: low
  float bandFloorMidDb_ = -24.0f;  // 3-band floor: mid
  float bandFloorHighDb_ = -24.0f; // 3-band floor: high
  float vadAdapt_ = 0.0f;          // VAD adaptation; 0 = off
  bool deltaMonitor_ = false;      // true = output dry-wet (removed)

  float inTrim_ = 1.0f;
  float outTrim_ = 1.0f;
  float mix_ = 0.0f;
  float mixTarget_ = 1.0f;
  bool bypassed_ = false;
  bool primed_ = false;
  bool outPrimed_ = false;  // wet output starts only after 2 frames buffered
  static constexpr uint32_t kPrimeFrames = 2;
  // Dry-path delay line (compensates wet-path latency so dry/wet mix aligns)
  std::vector<float> dryDelay_;
  uint32_t dryDelayLen_ = 0;
  uint32_t dryDelayWrite_ = 0;
  double consumePhase_ = 0.0;  // fractional model-sample consumption accumulator
  void calibrateDryDelay();
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

  DenoiseState* rnnoise_ = nullptr;
  SpeexResamplerState* resIn_ = nullptr;
  SpeexResamplerState* resOut_ = nullptr;
  RingBuffer* inRing_ = nullptr;
  RingBuffer* outRing_ = nullptr;
  float frameBuf_[kFrameSize] = {0.0f};
  float frameOutBuf_[kFrameSize] = {0.0f};
  float inScratch_[kScratchSamples] = {0.0f};
  float wetScratch_[kScratchSamples] = {0.0f};
  float outScratch_[kScratchSamples] = {0.0f};
  float pendingModel_[kScratchSamples] = {0.0f};
  float pendingHost_[kScratchSamples] = {0.0f};
  uint32_t pendingModelCount_ = 0;
  uint32_t pendingHostCount_ = 0;
};
