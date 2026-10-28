// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#include "DistrhoPlugin.hpp"
#include "dsp/DenoiserEngine.hpp"

START_NAMESPACE_DISTRHO

class NoiseOrdnance : public Plugin {
public:
  NoiseOrdnance()
    : Plugin(12 /* params */, 0 /* programs */, 0 /* states */)
  {
    engineL.init(getSampleRate());
    engineR.init(getSampleRate());
    // Defaults match the parameter definitions below (params 6 and 7).
    engineL.setSmoothing(0.5f);
    engineR.setSmoothing(0.5f);
    setLatency(engineL.latencySamples());
  }

  const char* getLabel() const override { return DISTRHO_PLUGIN_LABEL; }
  const char* getMaker() const override { return DISTRHO_PLUGIN_MAKER; }
  const char* getLicense() const override { return DISTRHO_PLUGIN_LICENSE; }
  uint32_t getVersion() const override { return d_version(1, 0, 0); }

  void initParameter(uint32_t index, Parameter& param) override {
    switch (index) {
      case 0:
        param.hints = kParameterIsAutomatable;
        param.name = "Reduction";
        param.symbol = "reduction";
        param.ranges.def = 100.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 100.0f;
        param.unit = "%";
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
        param.hints = kParameterIsAutomatable;
        param.name = "Output Trim";
        param.symbol = "output_trim";
        param.ranges.def = 0.0f;
        param.ranges.min = -12.0f;
        param.ranges.max = 12.0f;
        param.unit = "dB";
        break;
      case 3:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Bypass";
        param.symbol = "bypass";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 4:
        param.hints = kParameterIsAutomatable | kParameterIsBoolean;
        param.name = "Hum Filter";
        param.symbol = "hum_enable";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 5:
        param.hints = kParameterIsAutomatable | kParameterIsInteger;
        param.name = "Mains Freq";
        param.symbol = "hum_freq";
        param.ranges.def = 60.0f;
        param.ranges.min = 50.0f;
        param.ranges.max = 60.0f;
        param.unit = "Hz";
        break;
      case 6:
        param.hints = kParameterIsAutomatable;
        param.name = "Smoothing";
        param.symbol = "smoothing";
        param.ranges.def = 0.5f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 7:
        param.hints = kParameterIsAutomatable;
        param.name = "Floor Low";
        param.symbol = "floor_low";
        param.ranges.def = -24.0f;
        param.ranges.min = -80.0f;
        param.ranges.max = 0.0f;
        param.unit = "dB";
        break;
      case 8:
        param.hints = kParameterIsAutomatable;
        param.name = "Floor Mid";
        param.symbol = "floor_mid";
        param.ranges.def = -24.0f;
        param.ranges.min = -80.0f;
        param.ranges.max = 0.0f;
        param.unit = "dB";
        break;
      case 9:
        param.hints = kParameterIsAutomatable;
        param.name = "Floor High";
        param.symbol = "floor_high";
        param.ranges.def = -24.0f;
        param.ranges.min = -80.0f;
        param.ranges.max = 0.0f;
        param.unit = "dB";
        break;
      case 10:
        param.hints = kParameterIsAutomatable;
        param.name = "VAD Adapt";
        param.symbol = "vad_adapt";
        param.ranges.def = 0.0f;
        param.ranges.min = 0.0f;
        param.ranges.max = 1.0f;
        break;
      case 11:
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
    if (index < 12)
      return params[index];
    return 0.0f;
  }

  void setParameterValue(uint32_t index, float value) override {
    if (index >= 12)
      return;

    params[index] = value;

    switch (index) {
      case 0:
        engineL.setReduction(value / 100.0f);
        engineR.setReduction(value / 100.0f);
        break;
      case 1:
        engineL.setInputTrim(value);
        engineR.setInputTrim(value);
        break;
      case 2:
        engineL.setOutputTrim(value);
        engineR.setOutputTrim(value);
        break;
      case 3:
        engineL.setBypassed(value > 0.5f);
        engineR.setBypassed(value > 0.5f);
        break;
      case 4:
        engineL.setHum(value > 0.5f, params[5]);
        engineR.setHum(value > 0.5f, params[5]);
        break;
      case 5:
        {
          const float mains = value >= 55.0f ? 60.0f : 50.0f;
          engineL.setHum(params[4] > 0.5f, mains);
          engineR.setHum(params[4] > 0.5f, mains);
        }
        break;
      case 6:
        engineL.setSmoothing(value);
        engineR.setSmoothing(value);
        break;
      case 7:
      case 8:
      case 9:
        engineL.setBandFloor(params[7], params[8], params[9]);
        engineR.setBandFloor(params[7], params[8], params[9]);
        break;
      case 10:
        engineL.setVadAdapt(value);
        engineR.setVadAdapt(value);
        break;
      case 11:
        engineL.setDeltaMonitor(value > 0.5f);
        engineR.setDeltaMonitor(value > 0.5f);
        break;
      default:
        break;
    }
  }

  void activate() override {
    engineL.init(getSampleRate());
    engineR.init(getSampleRate());
    // Re-apply parameters (init resets engine state).
    engineL.setSmoothing(params[6]);
    engineR.setSmoothing(params[6]);
    engineL.setBandFloor(params[7], params[8], params[9]);
    engineR.setBandFloor(params[7], params[8], params[9]);
    engineL.setVadAdapt(params[10]);
    engineR.setVadAdapt(params[10]);
    engineL.setDeltaMonitor(params[11] > 0.5f);
    engineR.setDeltaMonitor(params[11] > 0.5f);
    engineL.setHum(params[4] > 0.5f, params[5] >= 55.0f ? 60.0f : 50.0f);
    engineR.setHum(params[4] > 0.5f, params[5] >= 55.0f ? 60.0f : 50.0f);
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
  float params[12] = {100.0f, 0.0f, 0.0f, 0.0f, 0.0f, 60.0f, 0.5f, -24.0f, -24.0f, -24.0f, 0.0f, 0.0f};

  DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoiseOrdnance)
};

Plugin* createPlugin() {
  return new NoiseOrdnance();
}

END_NAMESPACE_DISTRHO
