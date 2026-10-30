// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DenoiserEngine.hpp"

#include "RingBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <cstdarg>
#include <cstdio>

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


DenoiserEngine::DenoiserEngine() = default;

DenoiserEngine::~DenoiserEngine() {
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
  if (rnnoise_ != nullptr) {
    rnnoise_destroy(rnnoise_);
    rnnoise_ = nullptr;
  }
}

// Measures the wet-path latency by sending an impulse through at 100% wet
// and finding the output peak. The model suppresses the impulse (it's not
// speech-like) but the peak remains detectable as the global maximum in the
// search window. Runs at init time; ~1.5 s of audio at 44x realtime.
void DenoiserEngine::calibrateDryDelay() {
  const uint32_t rate = static_cast<uint32_t>(hostRate_);
  const uint32_t settle = rate;          // 1 s of zeros (mix/pipeline settle)
  const uint32_t tail = rate / 2;        // 0.5 s tail to catch the peak
  const uint32_t total = settle + 1 + tail;
  std::vector<float> in(total, 0.0f), out(total, 0.0f);
  in[settle] = 1.0f;

  const float savedReduction = reduction_;
  setReduction(1.0f);  // 100% wet: we measure the wet path only
  for (uint32_t d = 0; d < total;) {
    uint32_t n = std::min<uint32_t>(128, total - d);
    process(in.data() + d, out.data() + d, n);
    d += n;
  }

  float peak = 0.0f;
  uint32_t at = 0;
  for (uint32_t i = settle + 64; i < total; ++i) {
    const float a = std::fabs(out[i]);
    if (a > peak) { peak = a; at = i; }
  }
  if (at > settle && peak > 1e-4f) {
    dryDelayLen_ = at - settle;
  }
  // else: keep the initial guess.
  setReduction(savedReduction);
}

void DenoiserEngine::init(double hostSampleRate) {
  hostRate_ = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
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

  if (rnnoise_ != nullptr)
    rnnoise_destroy(rnnoise_);
  rnnoise_ = rnnoise_create(nullptr);
  setSmoothing(smoothing_);
  rnnoise_set_band_floor(rnnoise_, bandFloorLowDb_, bandFloorMidDb_, bandFloorHighDb_);
  rnnoise_set_vad_adapt(rnnoise_, vadAdapt_);

  // Dry-path delay: the wet path has a fixed latency D (priming + model
  // pipeline + resamplers). Delaying the dry signal by D keeps dry/wet
  // sample-aligned in the mix -- without it, middle mix positions produce
  // a D-sample echo (comb filtering). Start with a guess so the mix code has
  // a valid buffer, then measure the true D below.
  dryDelayLen_ = static_cast<uint32_t>(std::round(1456.0 * hostRate_ / 48000.0));
  // The wet-start gate is this guess, frozen before calibration: see wetStart_.
  wetStart_ = dryDelayLen_;
  dryDelay_.assign(dryDelayLen_ + 8192, 0.0f);
  dryDelayWrite_ = 0;

  reset();

  // Measure the true wet-path latency with an impulse probe, then size the
  // dry delay to match and clear the probe from the pipeline.
  calibrateDryDelay();
  dryDelay_.assign(dryDelayLen_ + 8192, 0.0f);
  dryDelayWrite_ = 0;
  reset();
}

void DenoiserEngine::reset() {
  if (inRing_ != nullptr)
    inRing_->reset();
  if (outRing_ != nullptr)
    outRing_->reset();

  if (resIn_ != nullptr)
    speex_resampler_reset_mem(resIn_);
  if (resOut_ != nullptr)
    speex_resampler_reset_mem(resOut_);
  if (rnnoise_ != nullptr)
    rnnoise_destroy(rnnoise_);

  rnnoise_ = rnnoise_create(nullptr);
  setSmoothing(smoothing_);
  rnnoise_set_band_floor(rnnoise_, bandFloorLowDb_, bandFloorMidDb_, bandFloorHighDb_);
  rnnoise_set_vad_adapt(rnnoise_, vadAdapt_);
  consumePhase_ = 0.0;
  outPos_ = 0;
  // The buffers are memset below, so the counts must go too -- otherwise a
  // reset mid-stream would leave stale counts pointing at zeroed buffers
  // and the first renders after reset would consume zeros as wet audio.
  pendingModelCount_ = 0;
  pendingHostCount_ = 0;
  dryDelayWrite_ = 0;
  if (dryDelayLen_ > 0)
    std::fill(dryDelay_.begin(), dryDelay_.end(), 0.0f);
  mix_ = 0.0f;

  mixTarget_ = reduction_;
  std::memset(frameBuf_, 0, sizeof(frameBuf_));
  std::memset(frameOutBuf_, 0, sizeof(frameOutBuf_));
  std::memset(inScratch_, 0, sizeof(inScratch_));
  std::memset(wetScratch_, 0, sizeof(wetScratch_));
  std::memset(outScratch_, 0, sizeof(outScratch_));
  std::memset(pendingModel_, 0, sizeof(pendingModel_));
  std::memset(pendingHost_, 0, sizeof(pendingHost_));
  std::memset(humS1_, 0, sizeof(humS1_));
  std::memset(humS2_, 0, sizeof(humS2_));
  pendingModelCount_ = 0;
  pendingHostCount_ = 0;
}

void DenoiserEngine::setSmoothing(float s) {
  smoothing_ = s;
  float smooth = 0.7f * s;
  float alpha = 0.8f * s;
  debugLog("slider=%.2f -> smooth=%.3f alpha=%.3f\n", s, smooth, alpha);
  if (rnnoise_ != nullptr) {
    rnnoise_set_gain_smoothing(rnnoise_, smooth);
    rnnoise_set_alpha(rnnoise_, alpha);
  }
}



void DenoiserEngine::setBandFloor(float lowDb, float midDb, float highDb) {
  bandFloorLowDb_ = lowDb;
  bandFloorMidDb_ = midDb;
  bandFloorHighDb_ = highDb;
  if (rnnoise_ != nullptr)
    rnnoise_set_band_floor(rnnoise_, lowDb, midDb, highDb);
}

void DenoiserEngine::setVadAdapt(float adapt) {
  vadAdapt_ = adapt;
  if (rnnoise_ != nullptr)
    rnnoise_set_vad_adapt(rnnoise_, adapt);
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
  if (inRing_ == nullptr || outRing_ == nullptr || resIn_ == nullptr || resOut_ == nullptr || rnnoise_ == nullptr)
    init(hostRate_);

  std::fill(inScratch_, inScratch_ + kScratchSamples, 0.0f);
  std::fill(wetScratch_, wetScratch_ + kScratchSamples, 0.0f);
  std::fill(outScratch_, outScratch_ + kScratchSamples, 0.0f);

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

  // Feed the dry delay line every block (even while priming) so it holds
  // real audio history by the time the wet path starts.
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

  spx_uint32_t inLen = frames;
  spx_uint32_t modelLen = static_cast<spx_uint32_t>(std::ceil(static_cast<double>(frames) * (kModelRate / hostRate_)) + 8.0);
  if (modelLen > kScratchSamples)
    modelLen = kScratchSamples;

  speex_resampler_process_float(resIn_, 0, inScratch_, &inLen, wetScratch_, &modelLen);
  if (modelLen > 0)
    inRing_->push(wetScratch_, modelLen);

  while (inRing_ != nullptr && inRing_->available() >= kFrameSize) {
    const uint32_t popped = inRing_->pop(frameBuf_, kFrameSize);
    if (popped != kFrameSize)
      break;

    // RNNoise's operating point is int16 scale (+-32768), exactly like
    // rnnoise_demo -- NOT +-1.0. Its internal silence gate (E < 0.04) is
    // calibrated for that scale: feeding +-1.0 floats makes quiet audio
    // take the silence path, i.e. silent passthrough with no denoising.
    for (uint32_t s = 0; s < kFrameSize; ++s)
      frameBuf_[s] *= 32768.0f;
    rnnoise_process_frame(rnnoise_, frameOutBuf_, frameBuf_);
    for (uint32_t s = 0; s < kFrameSize; ++s)
      frameOutBuf_[s] /= 32768.0f;
    outRing_->push(frameOutBuf_, kFrameSize);
  }

  // No whole-block priming gate here: what decides wet vs dry is the
  // absolute output position, not a call boundary (see the render loop
  // below). A gate that flips at a call boundary flips at a different
  // absolute sample for different block sizes, which by itself breaks
  // block-size invariance. The dry prefix before the fixed wet-start
  // position gives the pipeline its headroom instead.

  // Model-rate samples needed to render this block. Pop EXACTLY this many:
  // the ring absorbs the model's 480-sample chunkiness, so over-popping only
  // discards denoised audio (the old code popped everything available and
  // threw away whatever didn't fit this block -- starving the wet path).
  const double modelRatio = static_cast<double>(kModelRate) / hostRate_;
  // Fractional consumption: the model produces frames*modelRatio samples per
  // block ON AVERAGE (e.g. 139.32 at 44.1 kHz). Ceil()'ing to 140 every block
  // over-consumes by 0.68/block and slowly drains the ring. Accumulate the
  // fraction so long-term consumption exactly matches production.
  consumePhase_ += frames * modelRatio;
  const uint32_t intendedModel = static_cast<uint32_t>(consumePhase_);
  const uint32_t availModel = static_cast<uint32_t>(outRing_->available());
  uint32_t takeModel = std::min(intendedModel, availModel);
  // Guard: never overflow the scratch buffers, even for huge blocks.
  if (takeModel > kScratchSamples - pendingModelCount_)
    takeModel = kScratchSamples - pendingModelCount_;
  // Subtract only what was actually popped. The old code subtracted the
  // intended amount BEFORE capping, so any shortfall (ring starved or
  // scratch full) was lost from the accounting and the wet stream fell
  // permanently behind the output timeline -- by a chunk-dependent amount,
  // which is what made output block-size dependent. Retaining the debt
  // makes the next takes catch up instead of losing samples.
  consumePhase_ -= takeModel;

  // Prepend any model samples carried from the last block, then pop fresh ones.
  // (Dedicated pendingModel_ buffer: wetScratch_ is clobbered by the input
  // resampler at the top of every process() call.)
  if (takeModel > 0) {
    outRing_->pop(pendingModel_ + pendingModelCount_, takeModel);
    pendingModelCount_ += takeModel;
  }

  // Top the wet buffer up on every call instead of waiting for it to empty.
  // The old refill-when-empty policy let the buffer sawtooth down to zero:
  // whenever the model ring starved for a few calls (bursty 480-sample
  // production vs smooth consumption), takes got capped, the refill came
  // up short, and rendering fell back to dry for a chunk-dependent run of
  // samples -- which is what made output block-size dependent. Topping up
  // keeps the buffer near its prefix level, so it never empties in steady
  // state. This is safe because the resampler's output stream depends only
  // on its input stream, not on how that input is chunked across calls
  // (verified for regular and irregular chunkings at 1:1 and at 44.1/96 kHz
  // ratios) -- resampling earlier changes when samples are produced, never
  // which samples.
  if (pendingModelCount_ > 0 && pendingHostCount_ < kScratchSamples) {
    spx_uint32_t modelIn = pendingModelCount_;
    spx_uint32_t hostOut = kScratchSamples - pendingHostCount_;
    speex_resampler_process_float(resOut_, 0, pendingModel_, &modelIn,
                                  pendingHost_ + pendingHostCount_, &hostOut);
    pendingHostCount_ += hostOut;
    // Carry any model samples the resampler didn't consume (transients only
    // in steady state) instead of dropping them.
    const uint32_t usedModel = modelIn;
    if (usedModel < pendingModelCount_) {
      std::memmove(pendingModel_, pendingModel_ + usedModel,
                   (pendingModelCount_ - usedModel) * sizeof(float));
    }
    pendingModelCount_ -= usedModel;
  }

  // Smoothed wet/dry mix (~10 ms time constant -- no zipper noise on moves).
  const float target = bypassed_ ? 0.0f : mixTarget_;
  const float coeff =
      1.0f - std::exp(-1.0f / (0.010f * static_cast<float>(hostRate_)));

  // Render one sample at a time, in absolute output position. What a
  // sample sounds like depends only on outPos_, never on where a call
  // boundary fell:
  //   - before the fixed wet-start position: delayed dry (no wet is
  //     rendered yet). This dry prefix is also the pipeline's headroom.
  //   - after that: mixed wet/dry, wet strictly in order from pendingHost_.
  //     The dry sample comes from the measured latency back, so dry and
  //     wet stay sample-aligned: the measured latency is wet-start +
  //     chain lag, and wet[n - wetStart] is the denoised version of
  //     input[n - latency].
  //   - if the wet path ever runs short mid-stream (shouldn't in steady
  //     state): delayed dry, the same content a mix of 0 would give, so
  //     the fallback can't introduce a chunk-dependent split. The old
  //     tail emitted UNDELAYED dry, which differed from the mix path's
  //     delayed dry by the full latency on every affected sample.
  // mix_ advances only once the wet-start prefix is past (it used to freeze
  // during the old whole-block gate), so the wet onset still gets its
  // ~10 ms crossfade; mid-stream starved samples keep advancing it, so
  // re-entry stays click-free. It also snaps to target once within float
  // epsilon, so steady-state output is exactly wet (or exactly dry)
  // instead of carrying a ~1e-6 dry leak forever.
  const uint32_t dsize = static_cast<uint32_t>(dryDelay_.size());
  uint32_t wetIdx = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    // Dry sample from D ago: input written at (write - frames + i), minus D.
    const uint32_t didx =
        (dryDelayWrite_ + 2 * dsize - frames + i - dryDelayLen_) % dsize;
    const float dry = dryDelay_[didx];
    float outSample;
    if (outPos_ < wetStart_) {
      outSample = dry * outTrim_;
    } else {
      mix_ += (target - mix_) * coeff;
      if (std::fabs(target - mix_) < 1e-6f)
        mix_ = target;
      if (wetIdx < pendingHostCount_) {
        const float wet = pendingHost_[wetIdx++];
        const float mixed = dry * (1.0f - mix_) + wet * mix_;
        if (deltaMonitor_) {
          /* Delta monitor: output what was removed (dry - wet). */
          outSample = (dry - mixed) * outTrim_;
        } else {
          outSample = mixed * outTrim_;
        }
      } else {
        outSample = dry * outTrim_;
      }
    }
    out[i] = outSample;
    ++outPos_;
  }
  if (wetIdx > 0) {
    std::memmove(pendingHost_, pendingHost_ + wetIdx,
                 (pendingHostCount_ - wetIdx) * sizeof(float));
    pendingHostCount_ -= wetIdx;
  }
}

uint32_t DenoiserEngine::latencySamples() const {
  // The true wet-path latency, measured at init() by calibrateDryDelay().
  // Report it so the host can delay-compensate the plugin's output.
  return dryDelayLen_;
}
