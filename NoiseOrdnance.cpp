// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DistrhoPlugin.hpp"
#include "dsp/DenoiserEngine.hpp"

#include <cstdlib>
#include <string>

START_NAMESPACE_DISTRHO

// Model dropdown values. Statically allocated on purpose: DPF copies the
// Parameter (including this pointer) when initParameter returns, and the
// copy takes deleteLater=false from here, so nobody frees this array.
// The values must stay in the same order as DenoiserEngine::kModelDF3 /
// kModelDF3LL and dsp/DenoiserEngine.cpp's file table. DeepFilterNet2
// is not offered: it does not load (see DenoiserEngine.hpp).
static ParameterEnumerationValue kModelValues[] = {
    ParameterEnumerationValue(0.0f, "DeepFilterNet3"),
    ParameterEnumerationValue(1.0f, "DeepFilterNet3 Low Latency"),
};

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

  void initParameter(uint32_t index, Parameter& param) override {
    switch (index) {
      case 0:
        // Model selection: an enumeration parameter, so hosts render a
        // dropdown (LV2 scale points) instead of an integer stepper.
        // restrictedMode + >= 2 values is what makes the TTL generator
        // emit lv2:portProperty lv2:enumeration.
        param.hints = kParameterIsAutomatable | kParameterIsInteger;
        param.name = "Model";
        param.symbol = "model";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        param.enumValues.count = 2;
        param.enumValues.restrictedMode = true;
        param.enumValues.values = kModelValues;
        param.enumValues.deleteLater = false;
        break;
      case 1:
        param.hints = kParameterIsAutomatable;
        param.name = "Reduction";
        param.symbol = "reduction";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "%";
        break;
      case 2:
        param.hints = kParameterIsAutomatable;
        param.name = "Input Trim";
        param.symbol = "input_trim";
        param.ranges.def = 0.0f;
        param.ranges.min = -12.0f;
        param.ranges.max = 12.0f;
        param.unit = "dB";
        break;
      case 3:
        param.hints = kParameterIsAutomatable;
        param.name = "Output Trim";
        param.symbol = "output_trim";
        param.ranges.def = 0.0f;
        param.ranges.min = -12.0f;
        param.ranges.max = 12.0f;
        param.unit = "dB";
        break;
      case 4:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Bypass";
        param.symbol = "bypass";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 5:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Hum Filter";
        param.symbol = "hum_enable";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 6:
        param.hints = kParameterIsAutomatable | kParameterIsInteger;
        param.name = "Mains Freq";
        param.symbol = "hum_freq";
        param.ranges.def = 60.0f;
        param.ranges.min = 50.0f;
        param.ranges.max = 60.0f;
        param.unit = "Hz";
        break;
      case 7:
        param.hints = kParameterIsAutomatable;
        param.name = "Post Filter Beta";
        param.symbol = "post_filter_beta";
        param.ranges.def = 0.02f;
        param.ranges.min = 0.0f;
        param.ranges.max = 0.05f;
        break;
      case 8:
        param.hints = kParameterIsAutomatable;
        param.name = "Attenuation Limit";
        param.symbol = "atten_lim";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "dB";
        break;
      case 9:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Delta Monitor";
        param.symbol = "delta_monitor";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
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
        // Synchronous model reload (test branch: load time is not a
        // concern). Different models have different latencies, so the
        // host must be told the new value.
        if (engineL.setModel(static_cast<int>(value)))
          engineR.setModel(static_cast<int>(value));
        setLatency(engineL.latencySamples());
        break;
      case 1:
        engineL.setReduction(value / 100.0f);
        engineR.setReduction(value / 100.0f);
        break;
      case 2:
        engineL.setInputTrim(value);
        engineR.setInputTrim(value);
        break;
      case 3:
        engineL.setOutputTrim(value);
        engineR.setOutputTrim(value);
        break;
      case 4:
        engineL.setBypassed(value > 0.5f);
        engineR.setBypassed(value > 0.5f);
        break;
      case 5:
        engineL.setHum(value > 0.5f, params[6]);
        engineR.setHum(value > 0.5f, params[6]);
        break;
      case 6:
        {
          const float mains = value >= 55.0f ? 60.0f : 50.0f;
          engineL.setHum(params[5] > 0.5f, mains);
          engineR.setHum(params[5] > 0.5f, mains);
        }
        break;
      case 7:
        engineL.setPostFilterBeta(value);
        engineR.setPostFilterBeta(value);
        break;
      case 8:
        engineL.setAttenLim(value);
        engineR.setAttenLim(value);
        break;
      case 9:
        engineL.setDeltaMonitor(value > 0.5f);
        engineR.setDeltaMonitor(value > 0.5f);
        break;
      default:
        break;
    }
  }

  void activate() override {
    // Model files resolve in this order:
    //   1. $NOISE_ORDNANCE_MODEL_DIR (tests, harness, manual runs)
    //   2. <bundle>/models/ (installed LV2/VST3 bundles; the install
    //      scripts copy the tarballs there -- see BRANCH-NOTES.md)
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
    const int model = static_cast<int>(params[0]);
    engineL.init(getSampleRate(), modelDir, model);
    engineR.init(getSampleRate(), modelDir, model);
    // Re-apply parameters (init resets engine state; the model itself
    // was just loaded from params[0], so it is not re-selected here).
    engineL.setReduction(params[1] / 100.0f);
    engineR.setReduction(params[1] / 100.0f);
    engineL.setInputTrim(params[2]);
    engineR.setInputTrim(params[2]);
    engineL.setOutputTrim(params[3]);
    engineR.setOutputTrim(params[3]);
    engineL.setBypassed(params[4] > 0.5f);
    engineR.setBypassed(params[4] > 0.5f);
    engineL.setHum(params[5] > 0.5f, params[6] >= 55.0f ? 60.0f : 50.0f);
    engineR.setHum(params[5] > 0.5f, params[6] >= 55.0f ? 60.0f : 50.0f);
    engineL.setPostFilterBeta(params[7]);
    engineR.setPostFilterBeta(params[7]);
    engineL.setAttenLim(params[8]);
    engineR.setAttenLim(params[8]);
    engineL.setDeltaMonitor(params[9] > 0.5f);
    engineR.setDeltaMonitor(params[9] > 0.5f);
    setLatency(engineL.latencySamples());
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
  float params[10] = {0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 60.0f, 0.02f, 100.0f, 0.0f};

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseOrdnance)
};

Plugin* createPlugin() {
  return new NoiseOrdnance();
}

END_NAMESPACE_DISTRHO
