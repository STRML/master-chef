# OpenXR shell for the Halo Quest 3 port (Phase 4).
#
#   make        -> compile xr_shell.o xr_quads.o xr_lifecycle.o for
#                  arm64-v8a with the NDK (XR_USE_GRAPHICS_API_VULKAN +
#                  XR_USE_PLATFORM_ANDROID; links against the OpenXR
#                  loader in the APK).
#   make test   -> build and run the host unit tests (macOS clang,
#                  pure pose math + lifecycle FSM + quad stack; no
#                  OpenXR runtime calls).
#
# Override the NDK with NDK=/path/to/ndk.

NDK    ?= $(abspath ../../../../tools/.cache/android-ndk-r27c)
TC     := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64
CC     := $(TC)/bin/aarch64-linux-android29-clang

VISION := $(abspath ../../../../)
SRC    := $(abspath ../..)
XR     := $(abspath .)
XROBJ  := $(VISION)/native/build/android-arm64-xr

XR_INCLUDES := -I$(VISION)/third_party/openxr/include

# NativeActivity glue (android_main.c includes <android_native_app_glue.h>).
GLUE := $(NDK)/sources/android/native_app_glue

# Device build: full XR shell with Vulkan.
CFLAGS := -O2 -w -std=c11 -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 \
          -DXR_USE_PLATFORM_ANDROID=1 -DXR_USE_GRAPHICS_API_VULKAN=1 \
          -I. -I$(SRC) -I$(VISION)/native/EngineReuse -I$(GLUE) $(XR_INCLUDES)

OBJS := $(XROBJ)/xr_shell.o $(XROBJ)/xr_quads.o $(XROBJ)/xr_lifecycle.o \
        $(XROBJ)/android_main.o

.PHONY: all test clean

all: $(OBJS)

$(XROBJ):
	mkdir -p $(XROBJ)

$(XROBJ)/%.o: %.c xr.h $(XROBJ)
	$(CC) $(CFLAGS) -c $< -o $@

$(XROBJ)/%.o: $(SRC)/%.c xr.h $(XROBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Host unit tests: host clang (macOS) or gcc (halo-vk-dev container);
# Compile the pure modules (xr_quads.c, xr_lifecycle.c, section 1 of
# xr_shell.c) against the core OpenXR headers only; the runtime section
# of xr_shell.c is guarded by XR_USE_GRAPHICS_API_VULKAN and never
# compiled here.
HOSTCC ?= $(shell command -v clang >/dev/null 2>&1 && echo clang || echo gcc)
HOSTCFLAGS := -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
              -I. -I$(VISION)/third_party/openxr/include
TEST_BIN := $(XROBJ)/xr_tests

test: $(XROBJ)
	$(HOSTCC) $(HOSTCFLAGS) -o $(TEST_BIN) xr_tests.c xr_quads.c xr_lifecycle.c xr_shell.c -lm
	$(TEST_BIN)

clean:
	rm -rf $(XROBJ)
