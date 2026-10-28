# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — DPF plugin build

NAME = NoiseOrdnance

RNNOISE_SOURCES = \
	third_party/rnnoise/src/celt_lpc.c \
	third_party/rnnoise/src/denoise.c \
	third_party/rnnoise/src/kiss_fft.c \
	third_party/rnnoise/src/nnet.c \
	third_party/rnnoise/src/nnet_default.c \
	third_party/rnnoise/src/parse_lpcnet_weights.c \
	third_party/rnnoise/src/pitch.c \
	third_party/rnnoise/src/rnn.c \
	third_party/rnnoise/src/rnnoise_data.c \
	third_party/rnnoise/src/rnnoise_tables.c

FILES_DSP = \
	NoiseOrdnance.cpp \
	dsp/DenoiserEngine.cpp \
	$(RNNOISE_SOURCES)

TARGETS += lv2_sep
TARGETS += vst3

all: $(TARGETS)

include third_party/dpf/Makefile.plugins.mk

SPEEXDSP_CFLAGS := $(shell pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")
SPEEXDSP_LIBS := $(shell pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")

BUILD_C_FLAGS += -I. -Ithird_party/rnnoise/include -Ithird_party/rnnoise/src -Ithird_party/rnnoise/src/x86 $(SPEEXDSP_CFLAGS)
BUILD_C_FLAGS += -DFLOAT_APPROX
BUILD_CXX_FLAGS += -I. -Ithird_party/rnnoise/include -Ithird_party/rnnoise/src -Ithird_party/rnnoise/src/x86 $(SPEEXDSP_CFLAGS)
BUILD_CXX_FLAGS += -DFLOAT_APPROX
EXTRA_DSP_LIBS += -lm $(SPEEXDSP_LIBS)

.PHONY: all
