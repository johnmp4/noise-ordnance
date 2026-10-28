// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

// denoise_wav: offline file harness for the Noise Ordnance DSP core.
// Reads a 16-bit PCM mono WAV, runs it through DenoiserEngine in DAW-like
// blocks, writes a 16-bit PCM mono WAV. No DPF, no GUI, no audio device.
//
// Usage: denoise_wav in.wav out.wav [blocksize] [reduction01] [hum]
//   blocksize   default 128 (typical DAW block)
//   reduction01 default 1.0 (100%)
//   hum         default 0 (off); 50 or 60 enables the hum notch module
//
// This is the regression entry point: tests/run_drills.sh drives it over the
// drill files and tests/measure.py checks the numbers. If you change the DSP,
// run the drills -- a synth tone is NOT a valid test signal for this model.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DenoiserEngine.hpp"

namespace {

bool readU16(FILE* f, uint16_t& v) { return std::fread(&v, 2, 1, f) == 1; }
bool readU32(FILE* f, uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; }

struct Wav {
  uint32_t sampleRate = 0;
  std::vector<float> samples;  // mono, +-1.0
};

bool readWav(const char* path, Wav& wav, std::string& err) {
  FILE* f = std::fopen(path, "rb");
  if (!f) { err = "cannot open input"; return false; }

  char riff[4];
  uint32_t riffSize, dataSize = 0;
  uint16_t audioFormat = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0;
  long dataPos = -1;

  if (std::fread(riff, 1, 4, f) != 4 || std::memcmp(riff, "RIFF", 4) != 0)
    { err = "not a RIFF file"; std::fclose(f); return false; }
  if (!readU32(f, riffSize)) { err = "truncated header"; std::fclose(f); return false; }
  if (std::fread(riff, 1, 4, f) != 4 || std::memcmp(riff, "WAVE", 4) != 0)
    { err = "not a WAVE file"; std::fclose(f); return false; }

  // Walk chunks; we need fmt and data.
  while (dataPos < 0 || std::feof(f) == 0) {
    char id[4];
    uint32_t size;
    if (std::fread(id, 1, 4, f) != 4) break;
    if (!readU32(f, size)) break;
    const long chunkStart = std::ftell(f);
    if (std::memcmp(id, "fmt ", 4) == 0) {
      uint16_t fmtSize;
      if (!readU16(f, audioFormat)) break;
      if (!readU16(f, channels)) break;
      if (!readU32(f, sampleRate)) break;
      uint32_t byteRate; uint16_t blockAlign;
      if (!readU32(f, byteRate)) break;
      if (!readU16(f, blockAlign)) break;
      if (!readU16(f, bits)) break;
      (void)fmtSize;
    } else if (std::memcmp(id, "data", 4) == 0) {
      dataPos = chunkStart;
      dataSize = size;
      break;
    }
    std::fseek(f, chunkStart + ((size + 1) & ~1u), SEEK_SET);
  }

  if (dataPos < 0) { err = "no data chunk"; std::fclose(f); return false; }
  if (audioFormat != 1) { err = "only PCM supported"; std::fclose(f); return false; }
  if (bits != 16) { err = "only 16-bit supported"; std::fclose(f); return false; }
  if (channels < 1) { err = "no channels"; std::fclose(f); return false; }

  std::fseek(f, dataPos, SEEK_SET);
  const size_t frames = dataSize / (channels * 2);
  wav.samples.resize(frames);
  std::vector<int16_t> tmp(frames * channels);
  if (std::fread(tmp.data(), 2, frames * channels, f) != frames * channels)
    { err = "short data"; std::fclose(f); return false; }
  std::fclose(f);

  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (uint16_t c = 0; c < channels; ++c)
      sum += tmp[i * channels + c];
    wav.samples[i] = static_cast<float>((sum / channels) / 32768.0);
  }
  wav.sampleRate = sampleRate;
  return true;
}

bool writeWav(const char* path, const Wav& wav, std::string& err) {
  FILE* f = std::fopen(path, "wb");
  if (!f) { err = "cannot open output"; return false; }
  const uint32_t frames = static_cast<uint32_t>(wav.samples.size());
  const uint32_t dataBytes = frames * 2;
  const uint32_t riffSize = 36 + dataBytes;

  std::fwrite("RIFF", 1, 4, f);
  std::fwrite(&riffSize, 4, 1, f);
  std::fwrite("WAVE", 1, 4, f);
  std::fwrite("fmt ", 1, 4, f);
  const uint32_t fmtSize = 16;
  std::fwrite(&fmtSize, 4, 1, f);
  const uint16_t pcm = 1, mono = 1, bits = 16;
  std::fwrite(&pcm, 2, 1, f);
  std::fwrite(&mono, 2, 1, f);
  std::fwrite(&wav.sampleRate, 4, 1, f);
  const uint32_t byteRate = wav.sampleRate * 2;
  std::fwrite(&byteRate, 4, 1, f);
  const uint16_t blockAlign = 2;
  std::fwrite(&blockAlign, 2, 1, f);
  std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f);
  std::fwrite(&dataBytes, 4, 1, f);
  for (float s : wav.samples) {
    double v = s * 32768.0;
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    const int16_t q = static_cast<int16_t>(v);
    std::fwrite(&q, 2, 1, f);
  }
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s in.wav out.wav [blocksize] [reduction01] [hum]\n", argv[0]);
    return 2;
  }
  const uint32_t block = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 128;
  const float reduction = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 1.0f;
  const float humHz = argc > 5 ? static_cast<float>(std::atof(argv[5])) : 0.0f;
  if (block == 0 || block > 8192) {
    std::fprintf(stderr, "blocksize must be 1..8192\n");
    return 2;
  }

  Wav in;
  std::string err;
  if (!readWav(argv[1], in, err)) {
    std::fprintf(stderr, "read error: %s (%s)\n", err.c_str(), argv[1]);
    return 1;
  }

  DenoiserEngine engine;
  engine.init(static_cast<double>(in.sampleRate));
  engine.setReduction(reduction);
  if (humHz > 0.0f)
    engine.setHum(true, humHz);

  Wav out;
  out.sampleRate = in.sampleRate;
  out.samples.resize(in.samples.size());

  size_t done = 0;
  const size_t total = in.samples.size();
  while (done < total) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(block, total - done));
    engine.process(in.samples.data() + done, out.samples.data() + done, n);
    done += n;
  }

  if (!writeWav(argv[2], out, err)) {
    std::fprintf(stderr, "write error: %s (%s)\n", err.c_str(), argv[2]);
    return 1;
  }
  std::fprintf(stderr, "denoise_wav: %zu samples @ %u Hz, block %u, reduction %.2f -> %s\n",
               total, in.sampleRate, block, reduction, argv[2]);
  return 0;
}
