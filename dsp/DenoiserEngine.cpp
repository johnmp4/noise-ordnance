// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DenoiserEngine.hpp"

#include "RingBuffer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

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

// Model files (in the model directory) and dropdown labels, indexed by
// DenoiserEngine::kModelDF3 / kModelDF3LL. The plugin's Model
// enumeration parameter uses the same order. DeepFilterNet2 is not
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
    5616,  // DeepFilterNet3 -- measured by correlation, see BRANCH-NOTES.md
    4656,  // DeepFilterNet3 Low Latency -- measured by correlation
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
bool DenoiserEngine::loadModelFile(const std::string& path) {
  if (!fileExists(path))
    return false;
  DFState* next = df_create(path.c_str(), attenLimDb_, nullptr);
  if (next == nullptr)
    return false;
  const size_t fl = df_get_frame_length(next);
  if (fl == 0 || fl > 4096) {
    df_free(next);
    return false;
  }
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
  pendingModelCount_ = 0;
  pendingHostCount_ = 0;
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
  // keeps the previous state (see loadModelFile).
  if (initialized_ && !modelDir_.empty())
    loadModelFile(modelDir_ + "/" + modelFileName(modelIndex_));
  clearPipeline();
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
  if (!loadModelFile(modelDir_ + "/" + modelFileName(index)))
    return false;  // keeps the current model
  modelIndex_ = index;
  latency48k_ = kModelLatency48k[index];
  dryDelayLen_ = static_cast<uint32_t>(std::round(latency48k_ * hostRate_ / 48000.0));
  clearPipeline();
  return true;
}

void DenoiserEngine::setPostFilterBeta(float beta) {
  postFilterBeta_ = beta;
  if (dfState_ != nullptr)
    df_set_post_filter_beta(dfState_, beta);
}

void DenoiserEngine::setAttenLim(float db) {
  attenLimDb_ = db;
  if (dfState_ != nullptr)
    df_set_atten_lim(dfState_, db);
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
  // Very large blocks would overflow the fixed scratch buffers: split
  // them into scratch-sized chunks (identical to the host calling with
  // smaller blocks, which this engine is invariant to -- see the
  // block-invariance unit test).
  if (frames > kScratchSamples) {
    uint32_t done = 0;
    while (done < frames) {
      const uint32_t n = std::min(kScratchSamples, frames - done);
      process(in + done, out + done, n);
      done += n;
    }
    return;
  }
  if (!initialized_) {
    // Never init()'d (e.g. plugin constructed but not activated):
    // true passthrough. No model load is attempted from the audio
    // thread -- and, importantly, not during TTL generation either,
    // which instantiates the plugin without activating it.
    for (uint32_t i = 0; i < frames; ++i)
      out[i] = in[i] * outTrim_;
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
  if (modelLen > 0)
    inRing_->push(wetScratch_, modelLen);

  // Run whole DF frames. DeepFilterNet takes float audio at +-1.0
  // (unlike RNNoise's int16-scale operating point) -- no scaling here.
  // The DF state buffers lookahead internally, so output frames emerge
  // delayed by the model's latency; that delay is what the measured
  // latency table accounts for.
  if (dfState_ != nullptr && frameLen_ > 0) {
    while (inRing_->available() >= frameLen_) {
      const uint32_t popped = inRing_->pop(frameBuf_.data(), frameLen_);
      if (popped != frameLen_)
        break;
      df_process_frame(dfState_, frameBuf_.data(), frameOutBuf_.data());
      outRing_->push(frameOutBuf_.data(), frameLen_);
    }
  }

  // Stage model-rate wet samples for the output resampler, up to scratch
  // room. Popping into pendingModel_ (not straight into the resampler)
  // keeps the resampler's input stream contiguous across blocks.
  {
    const uint32_t room = kScratchSamples - pendingModelCount_;
    uint32_t take = static_cast<uint32_t>(outRing_->available());
    if (take > room)
      take = room;
    if (take > 0) {
      outRing_->pop(pendingModel_ + pendingModelCount_, take);
      pendingModelCount_ += take;
    }
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
    mix_ += (target - mix_) * coeff;
    if (std::fabs(target - mix_) < 1e-6f)
      mix_ = target;  // snap: no permanent ~1e-6 dry leak at steady state
    // Dry sample from dryDelayLen_ ago: input written at
    // (write - frames + i), minus the delay.
    const uint32_t didx =
        (dryDelayWrite_ + 2 * dsize - frames + i - dryDelayLen_) % dsize;
    const float dry = dryDelay_[didx];
    const uint64_t pos = streamPos_ + i;
    float outSample;
    if (pos < wetStart_ || wetUsed >= pendingHostCount_) {
      // Not yet wet, or starved: delayed dry (never undelayed dry --
      // that would shift the sample by the full latency).
      outSample = dry * outTrim_;
    } else {
      const float wet = pendingHost_[wetUsed++];
      const float mixed = dry * (1.0f - mix_) + wet * mix_;
      if (deltaMonitor_) {
        // Delta monitor: output what was removed (dry - wet).
        outSample = (dry - mixed) * outTrim_;
      } else {
        outSample = mixed * outTrim_;
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
