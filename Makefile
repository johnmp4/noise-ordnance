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
TARGETS += clap

all: $(TARGETS)

include third_party/dpf/Makefile.plugins.mk

# The plugin targets need the Rust static library first. (DPF's targets
# are phony, so this prerequisite just orders the libDF build ahead of
# the link -- it does not change what DPF builds.)
lv2_sep vst3 clap: $(DF_STATICLIB)

$(DF_STATICLIB):
	bash third_party/build-libdf.sh

SPEEXDSP_CFLAGS := $(shell pkg-config --cflags speexdsp 2>/dev/null || echo "-I/usr/include")
SPEEXDSP_LIBS := $(shell pkg-config --libs speexdsp 2>/dev/null || echo "-lspeexdsp")
ifneq ($(WINDOWS),true)
# Linux only: link speexdsp statically so the release bundle is
# copy-to-folder with no libspeexdsp1 package needed on the user's
# system. -Bstatic forces the archive choice (the linker otherwise
# prefers the .so), so a distro package without a static archive
# fails the build loudly instead of silently shipping a dynamic
# dependency. Only speex is wrapped: static glibc libm breaks
# shared-library links (IFUNC references), and the trailing -lm
# covers speex's private libm dep dynamically — it must come AFTER
# speex, since static linking resolves symbols in link order
# (unlike dynamic linking, which defers them to runtime).
# Windows is untouched: its build already links speex statically
# (verified via objdump).
SPEEXDSP_LIBS := -Wl,-Bstatic -lspeexdsp -Wl,-Bdynamic -lm
endif

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
# A Rust staticlib needs these system libs for std on Windows: Winsock
# (networking), NT native API, BCrypt (randomness) and userenv (user
# profile directory). They must come after libdf.a in link order, which
# holds because this appends after the line above.
ifeq ($(WINDOWS),true)
EXTRA_DSP_LIBS += -lws2_32 -lntdll -lbcrypt -luserenv
endif

.PHONY: all



