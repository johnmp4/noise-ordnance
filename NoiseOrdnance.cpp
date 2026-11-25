// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DistrhoPlugin.hpp"
#include "dsp/DenoiserEngine.hpp"

#include <cstdlib>
#include <string>

START_NAMESPACE_DISTRHO

// Model selection is a boolean switch, not a dropdown: with only two
// loadable models, a toggle is clearer than an enum whose values a host
// may render unlabeled. OFF (0) = DeepFilterNet3, the post-production
// model (default); ON (1) = DeepFilterNet3 Low Latency (no lookahead).
// The mapping lives here, in the plugin: the engine still takes a model
// index (DenoiserEngine::kModelDF3 / kModelDF3LL), and the harness and
// unit tests still address models by index directly. DeepFilterNet2
// is not offered: it does not load (see DenoiserEngine.hpp).
// If a third loadable model ever appears, this switch should become an
// enumeration again -- a toggle cannot express three choices.

class NoiseOrdnance : public Plugin {
public:
  NoiseOrdnance()
    : Plugin(10 /* params */, 0 /* programs */, 0 /* states */)
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
  //   Bypass, Input Trim, Hum Filter, Hum Freq, Low Latency Model,
  //   Reduction, Aggressiveness, Postfilter Strength, Delta Monitor,
  //   Output Trim
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
        // Low Latency Model switch: OFF = DeepFilterNet3 (post-production,
        // default), ON = DeepFilterNet3 Low Latency (no lookahead). A
        // boolean renders as a labeled toggle in hosts instead of an
        // enum that may show up as an unlabeled binary switch.
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Low Latency Model";
        param.symbol = "low_latency";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 5:
        param.hints = kParameterIsAutomatable;
        param.name = "Reduction";
        param.symbol = "reduction";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "%";
        break;
      case 6:
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
      case 7:
        // Postfilter Strength: the post-filter beta, renamed so the
        // control reads as a strength. 0 = post-filter off.
        param.hints = kParameterIsAutomatable;
        param.name = "Postfilter Strength";
        param.symbol = "post_filter_beta";
        param.ranges.def = 0.02f;
        param.ranges.min = 0.0f;
        param.ranges.max = 0.05f;
        break;
      case 8:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Delta Monitor";
        param.symbol = "delta_monitor";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 9:
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
    if (index < 10)
      return params[index];
    return 0.0f;
  }

  void setParameterValue(uint32_t index, float value) override {
    if (index >= 10)
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
        // Synchronous model reload (test branch: load time is not a
        // concern). Switch ON = low-latency model, OFF = post-production
        // model. Different models have different latencies, so the
        // host must be told the new value.
        if (engineL.setModel(value > 0.5f ? DenoiserEngine::kModelDF3LL : DenoiserEngine::kModelDF3))
          engineR.setModel(value > 0.5f ? DenoiserEngine::kModelDF3LL : DenoiserEngine::kModelDF3);
        setLatency(engineL.latencySamples());
        break;
      case 5:
        engineL.setReduction(value / 100.0f);
        engineR.setReduction(value / 100.0f);
        break;
      case 6:
        engineL.setAttenLim(value);
        engineR.setAttenLim(value);
        break;
      case 7:
        engineL.setPostFilterBeta(value);
        engineR.setPostFilterBeta(value);
        break;
      case 8:
        engineL.setDeltaMonitor(value > 0.5f);
        engineR.setDeltaMonitor(value > 0.5f);
        break;
      case 9:
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
    //      scripts copy the tarballs there)
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
    const int model = params[4] > 0.5f ? DenoiserEngine::kModelDF3LL : DenoiserEngine::kModelDF3;
    engineL.init(getSampleRate(), modelDir, model);
    engineR.init(getSampleRate(), modelDir, model);
    // Re-apply parameters (init resets engine state; the model itself
    // was just loaded from params[4], so it is not re-selected here).
    engineL.setBypassed(params[0] > 0.5f);
    engineR.setBypassed(params[0] > 0.5f);
    engineL.setInputTrim(params[1]);
    engineR.setInputTrim(params[1]);
    engineL.setHum(params[2] > 0.5f, params[3] >= 55.0f ? 60.0f : 50.0f);
    engineR.setHum(params[2] > 0.5f, params[3] >= 55.0f ? 60.0f : 50.0f);
    engineL.setReduction(params[5] / 100.0f);
    engineR.setReduction(params[5] / 100.0f);
    engineL.setAttenLim(params[6]);
    engineR.setAttenLim(params[6]);
    engineL.setPostFilterBeta(params[7]);
    engineR.setPostFilterBeta(params[7]);
    engineL.setDeltaMonitor(params[8] > 0.5f);
    engineR.setDeltaMonitor(params[8] > 0.5f);
    engineL.setOutputTrim(params[9]);
    engineR.setOutputTrim(params[9]);
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
  // Order: Bypass, In Trim, Hum Filter, Hum Freq, Low Latency, Reduction,
  // Aggressiveness, Postfilter, Delta, Out Trim.
  float params[10] = {0.0f, 0.0f, 0.0f, 60.0f, 0.0f, 100.0f, 100.0f, 0.02f, 0.0f, 0.0f};

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseOrdnance)
};

Plugin* createPlugin() {
  return new NoiseOrdnance();
}

END_NAMESPACE_DISTRHO
