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

# Use DPF's PKG_CONFIG here, not plain `pkg-config`: DPF defaults it to
# `pkg-config --static` on Windows and plain `pkg-config` elsewhere.
# The project previously bypassed that default, so Windows builds
# linked the speex DLL. With this, Windows links speexdsp statically
# when the package ships a static archive (verify with objdump: no
# libspeexdsp-1.dll in the import table), while Linux behavior is
# unchanged. Goal: a release bundle with no speex DLL to install
# next to it.
SPEEXDSP_CFLAGS := $(shell $(PKG_CONFIG) --cflags speexdsp 2>/dev/null || echo "-I/usr/include")
SPEEXDSP_LIBS := $(shell $(PKG_CONFIG) --libs speexdsp 2>/dev/null || echo "-lspeexdsp")

BUILD_C_FLAGS += -I. $(SPEEXDSP_CFLAGS)
BUILD_CXX_FLAGS += -I. $(SPEEXDSP_CFLAGS)
# The model-switch race fix uses std::shared_mutex, which needs C++17.
# DPF defaults the plugin build to -std=gnu++11, so pin gnu++17 here;
# GCC honors the last -std on the command line, and this append lands
# after DPF's, so it wins. (The unit tests already build at C++17,
# which is why they passed while the plugin build failed.)
BUILD_CXX_FLAGS += -std=gnu++17
# A Rust staticlib needs the pthread/dl libs for std on Linux.
EXTRA_DSP_LIBS += -lm $(SPEEXDSP_LIBS) -lpthread -ldl $(DF_STATICLIB)

.PHONY: all

