// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DistrhoPlugin.hpp"
#include "dsp/DenoiserEngine.hpp"

#include <cstdlib>
#include <string>

START_NAMESPACE_DISTRHO

// Single model: DeepFilterNet3, the post-production model with
// lookahead. A low-latency variant (DeepFilterNet3 Low Latency, no
// lookahead, 20 ms less delay) is deliberately not offered: the 20 ms
// does not change any use case this plugin targets, and the variant
// costs about twice the CPU and 4.5x the download size.
// DeepFilterNet2 is not offered either: it does not load
// (see DenoiserEngine.hpp).

class NoiseOrdnance : public Plugin {
public:
  NoiseOrdnance()
    : Plugin(9 /* params */, 0 /* programs */, 0 /* states */)
  {
    // NOTE: engines are deliberately NOT init()'d here. TTL generation
    // instantiates the plugin without activating it, and init() loads a
    // model file -- a missing file at TTL time must not crash the build.
    // Engines load in activate(), below. Until then, process() passes
    // dry audio through (see DenoiserEngine::process).
  }

  const char* getLabel() const override { return DISTRHO_PLUGIN_LABEL; }
  const char* getMaker() const override { return DISTRHO_PLUGIN_MAKER; }
  const char* getLicense() const override { return DISTRHO_PLUGIN_LICENSE; }
  uint32_t getVersion() const override { return d_version(1, 0, 0); }

  // Parameter order (hosts display in index order): intermixed so related
  // parameters group in flat lists (Carla stacks all parameters in port
  // order). Ardour separates switches from sliders but preserves each
  // subsequence, so its two-column layout pairs the same groups:
  //   Bypass, Input Trim, Hum Filter, Hum Freq, Reduction,
  //   Aggressiveness, Postfilter Strength, Delta Monitor, Output Trim
  // Symbols are the stable automation IDs: only display names changed
  // (Mains Freq -> Hum Freq, Attenuation Limit -> Aggressiveness,
  // Post Filter Beta -> Postfilter Strength).
  void initParameter(uint32_t index, Parameter& param) override {
    switch (index) {
      case 0:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Bypass";
        param.symbol = "bypass";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 1:
        param.hints = kParameterIsAutomatable;
        param.name = "Input Trim";
        param.symbol = "input_trim";
        param.ranges.def = 0.0f;
        param.ranges.min = -12.0f;
        param.ranges.max = 12.0f;
        param.unit = "dB";
        break;
      case 2:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Hum Filter";
        param.symbol = "hum_enable";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 3:
        param.hints = kParameterIsAutomatable | kParameterIsInteger;
        param.name = "Hum Freq";
        param.symbol = "hum_freq";
        param.ranges.def = 60.0f;
        param.ranges.min = 50.0f;
        param.ranges.max = 60.0f;
        param.unit = "Hz";
        break;
      case 4:
        param.hints = kParameterIsAutomatable;
        param.name = "Reduction";
        param.symbol = "reduction";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "%";
        break;
      case 5:
        // Aggressiveness: the attenuation limit, renamed for direction.
        // The value is still max attenuation in dB: 0 = no attenuation
        // allowed, 100 = no cap. Right = the model cuts deeper.
        param.hints = kParameterIsAutomatable;
        param.name = "Aggressiveness";
        param.symbol = "atten_lim";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "dB";
        break;
      case 6:
        // Postfilter Strength: the post-filter beta, renamed so the
        // control reads as a strength. 0 = post-filter off.
        param.hints = kParameterIsAutomatable;
        param.name = "Postfilter Strength";
        param.symbol = "post_filter_beta";
        param.ranges.def = 0.02f;
        param.ranges.min = 0.0f;
        param.ranges.max = 0.05f;
        break;
      case 7:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Delta Monitor";
        param.symbol = "delta_monitor";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 8:
        param.hints = kParameterIsAutomatable;
        param.name = "Output Trim";
        param.symbol = "output_trim";
        param.ranges.def = 0.0f;
        param.ranges.min = -12.0f;
        param.ranges.max = 12.0f;
        param.unit = "dB";
        break;
      default:
        break;
    }
  }

  float getParameterValue(uint32_t index) const override {
    if (index < 9)
      return params[index];
    return 0.0f;
  }

  void setParameterValue(uint32_t index, float value) override {
    if (index >= 9)
      return;

    params[index] = value;

    switch (index) {
      case 0:
        engineL.setBypassed(value > 0.5f);
        engineR.setBypassed(value > 0.5f);
        break;
      case 1:
        engineL.setInputTrim(value);
        engineR.setInputTrim(value);
        break;
      case 2:
        engineL.setHum(value > 0.5f, params[3]);
        engineR.setHum(value > 0.5f, params[3]);
        break;
      case 3:
        {
          const float mains = value >= 55.0f ? 60.0f : 50.0f;
          engineL.setHum(params[2] > 0.5f, mains);
          engineR.setHum(params[2] > 0.5f, mains);
        }
        break;
      case 4:
        engineL.setReduction(value / 100.0f);
        engineR.setReduction(value / 100.0f);
        break;
      case 5:
        engineL.setAttenLim(value);
        engineR.setAttenLim(value);
        break;
      case 6:
        engineL.setPostFilterBeta(value);
        engineR.setPostFilterBeta(value);
        break;
      case 7:
        engineL.setDeltaMonitor(value > 0.5f);
        engineR.setDeltaMonitor(value > 0.5f);
        break;
      case 8:
        engineL.setOutputTrim(value);
        engineR.setOutputTrim(value);
        break;
      default:
        break;
    }
  }

  void activate() override {
    // Model files resolve in this order:
    //   1. $NOISE_ORDNANCE_MODEL_DIR (tests, harness, manual runs)
    //   2. <bundle>/models/ (installed LV2/VST3 bundles; the install
    //      scripts copy the model tarball there)
    // If neither yields a loadable file, the engines run dry
    // passthrough (no crash) until a valid directory appears.
    std::string modelDir;
    const char* env = std::getenv("NOISE_ORDNANCE_MODEL_DIR");
    if (env != nullptr && env[0] != '\0') {
      modelDir = env;
    } else {
      const char* bundle = getBundlePath();
      if (bundle != nullptr && bundle[0] != '\0')
        modelDir = std::string(bundle) + "/models";
    }
    // Single model: DeepFilterNet3, the post-production model with
    // lookahead. No model switch, so there is nothing to re-select.
    engineL.init(getSampleRate(), modelDir, DenoiserEngine::kModelDF3);
    engineR.init(getSampleRate(), modelDir, DenoiserEngine::kModelDF3);
    // Re-apply parameters (init resets engine state).
    engineL.setBypassed(params[0] > 0.5f);
    engineR.setBypassed(params[0] > 0.5f);
    engineL.setInputTrim(params[1]);
    engineR.setInputTrim(params[1]);
    engineL.setHum(params[2] > 0.5f, params[3] >= 55.0f ? 60.0f : 50.0f);
    engineR.setHum(params[2] > 0.5f, params[3] >= 55.0f ? 60.0f : 50.0f);
    engineL.setReduction(params[4] / 100.0f);
    engineR.setReduction(params[4] / 100.0f);
    engineL.setAttenLim(params[5]);
    engineR.setAttenLim(params[5]);
    engineL.setPostFilterBeta(params[6]);
    engineR.setPostFilterBeta(params[6]);
    engineL.setDeltaMonitor(params[7] > 0.5f);
    engineR.setDeltaMonitor(params[7] > 0.5f);
    engineL.setOutputTrim(params[8]);
    engineR.setOutputTrim(params[8]);
    setLatency(engineL.latencySamples());
    // Async inference: from here on, DF frames run on a worker thread
    // per channel, and the audio thread only resamples, shuffles
    // rings, and renders. This is what makes 128-sample blocks viable:
    // a frame's inference no longer has to fit a 2.7 ms audio deadline.
    // It adds no latency (the worker's budget is the existing wetStart
    // slack) and changes no reported latency. Tests and the offline
    // harness never start the worker, so they keep the synchronous path.
    engineL.startWorker();
    engineR.startWorker();
  }

  void deactivate() override {
    engineL.stopWorker();
    engineR.stopWorker();
  }

  void run(const float** inputs, float** outputs, uint32_t frames) override {
    if (inputs == nullptr || outputs == nullptr)
      return;

    engineL.process(inputs[0], outputs[0], frames);
    engineR.process(inputs[1], outputs[1], frames);
  }

private:
  DenoiserEngine engineL;
  DenoiserEngine engineR;
  // Order: Bypass, In Trim, Hum Filter, Hum Freq, Reduction,
  // Aggressiveness, Postfilter, Delta, Out Trim.
  float params[9] = {0.0f, 0.0f, 0.0f, 60.0f, 100.0f, 100.0f, 0.02f, 0.0f, 0.0f};

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseOrdnance)
};

Plugin* createPlugin() {
  return new NoiseOrdnance();
}

END_NAMESPACE_DISTRHO
