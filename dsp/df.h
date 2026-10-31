// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT
// df.h: hand-written C declarations for libDF's C API (libDF/src/capi.rs
// in Rikorose/DeepFilterNet, pinned at main commit d375b2d -- see
// third_party/install-third-party.sh). Kept in sync by hand: if libDF's
// capi.rs changes, update this file. The authoritative signatures live
// in capi.rs; this file exists so the C++ engine can call the API
// without needing a Rust toolchain at plugin build time (only at libDF
// build time, see third_party/build-libdf.sh).
//
// Only the functions the engine uses are declared here; capi.rs also
// exposes df_process_frame_raw, df_coef_size, and df_gain_size, which
// this plugin does not need.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DFState DFState;

// Create a DeepFilterNet state from a model tar.gz path.
//   path: file path to a DeepFilterNet .tar.gz ONNX model.
//   atten_lim: attenuation limit in dB (100 = no limit).
//   log_level: NULL (logging off) or a level string like "info".
// A missing or corrupt model path PANICS inside Rust (abort), so callers
// MUST stat-check the path before calling this -- see
// DenoiserEngine::loadModelFile.
DFState* df_create(const char* path, float atten_lim, const char* log_level);

// Frame size in samples (the model's hop size; 480 at 48 kHz for the
// DF3 models). Input and output buffers for df_process_frame must
// be exactly this long.
size_t df_get_frame_length(DFState* st);

// Next log message, if logging was enabled at creation. Free the
// returned string with df_free_log_msg. Returns NULL when no message
// is queued (or logging is off).
char* df_next_log_msg(DFState* st);
void df_free_log_msg(char* ptr);

// Attenuation limit in dB (see DenoiserEngine::setAttenLim).
void df_set_atten_lim(DFState* st, float lim_db);

// Post-filter beta, 0..0.05. A beta of 0 disables the post-filter.
void df_set_post_filter_beta(DFState* st, float beta);

// Process one frame of float audio (+-1.0). Input and output are
// df_get_frame_length() samples. Returns the frame's local SNR;
// callers may ignore it.
float df_process_frame(DFState* st, float* input, float* output);

// Free a state created by df_create.
void df_free(DFState* model);

#ifdef __cplusplus
}
#endif
