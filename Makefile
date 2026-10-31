# Copyright (c) 2026 John Peterman
# SPDX-License-Identifier: MIT
# Noise Ordnance — DPF plugin build (DeepFilterNet engine branch)
#
# The model core is libDF (Rust, Rikorose/DeepFilterNet), built as a
# static library by third_party/build-libdf.sh and linked here. The C++
# engine calls it through the plain C API declared in dsp/df.h -- no
# Rust code lives in this repo.

NAME = NoiseOrdnance

# libDF static library (built by third_party/build-libdf.sh; the
# install script runs it). CARGO_TARGET_DIR is pinned in that script so
# this path is stable regardless of cargo workspace layout.
DF_STATICLIB = third_party/deepfilternet/target/release/libdf.a

FILES_DSP = \
	NoiseOrdnance.cpp \
	dsp/DenoiserEngine.cpp

TARGETS += lv2_sep
TARGETS += vst3

all: $(TARGETS)

include third_party/dpf/Makefile.plugins.mk

# The plugin targets need the Rust static library first. (DPF's targets
# are phony, so this prerequisite just orders the libDF build ahead of
# the link -- it does not change what DPF builds.)
lv2_sep vst3: $(DF_STATICLIB)

$(DF_STATICLIB):
	bash third_party/build-libdf.sh

SPEEXDSP_CFLAGS := $(shell pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")
SPEEXDSP_LIBS := $(shell pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")

BUILD_C_FLAGS += -I. $(SPEEXDSP_CFLAGS)
BUILD_CXX_FLAGS += -I. $(SPEEXDSP_CFLAGS)
# A Rust staticlib needs the pthread/dl libs for std on Linux.
EXTRA_DSP_LIBS += -lm $(SPEEXDSP_LIBS) -lpthread -ldl $(DF_STATICLIB)

.PHONY: all
