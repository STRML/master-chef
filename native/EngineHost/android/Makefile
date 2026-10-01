# Android (arm64-v8a) headless build of the translated Halo engine host.
#
# Compiles the same generated engine chunks + portable host shims as the
# macOS `halo-host`, replacing the three ObjC/Metal modules (metalwin.m,
# metalrenderer.m, gamecontroller.m) and the macOS main.c with headless
# stubs in this directory. The real directsound.c is kept; its AudioToolbox
# include is redirected to fake_audioqueue.h (see that file + directsound_stub.c).
#
# Device audio (Phase 5, HALO_AAUDIO_OUTPUT): the Quest 3 device build
# links aaudio_output.c INSTEAD of directsound_stub.c. Same AudioQueue
# facade (fake_audioqueue.h), same threading contract, real sink: the
# worker's AAudioStream_write blocks on the hardware ring, supplying the
# cadence directsound.c relies on. Link with -lAAudio (API 26+; the NDK
# clang wrapper here targets API 29). `make aaudio-check` compiles the
# sink for arm64-v8a; host unit tests for its pure helpers:
# native/EngineHost/tests/test_aaudio_output.c.
#
# This build is additive: it touches no source outside android/ and the
# #ifdef __ANDROID__ guards, so `make -C .. halo-host` (macOS) is unchanged.
#
# Usage:
#   make                 -> ./halo-headless (arm64-v8a ELF)
#   make clean
# Override the NDK with NDK=/path/to/ndk if it lives elsewhere.

NDK    ?= $(abspath ../../../tools/.cache/android-ndk-r27c)
TC     := $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64
CC     := $(TC)/bin/aarch64-linux-android29-clang

VISION := $(abspath ../../..)
SRC    := $(abspath ..)
GEN    := $(VISION)/native/build/engine-reuse/whole-exe
OBJ    := $(VISION)/native/build/android-arm64

MOJODIR  := $(VISION)/third_party/mojoshader
MOJODEFS := -DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 \
            -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 \
            -DSUPPORT_PROFILE_GLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 \
            -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0

CFLAGS := -O2 -w -std=c11 -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 \
          -D__ANDROID__ -DHALO_NO_PTHREAD_CANCEL \
          -I. -I$(SRC) -I$(VISION)/native/EngineReuse -I$(GEN)

# Portable host shims (subset of the macOS HOST_SRCS): drop macOS main.c.
HOST_SRCS := host.c threading.c haptics.c halo_settings.c pointer.c \
             shims_kernel32.c shims_misc.c d3d9.c texture_decode.c metalshader.c \
             dinput8.c ddraw.c directsound.c directsound_mixer.c vorbis_shim.c \
             resources.c overrides.c
# Headless replacements for the ObjC modules + macOS main.
STUB_SRCS := main.c metalrenderer_stub.c gamecontroller_stub.c metalwin_stub.c \
             directsound_stub.c

CHUNKS      := $(wildcard $(GEN)/chunk_*.c)
CHUNK_OBJS  := $(patsubst $(GEN)/%.c,$(OBJ)/%.o,$(CHUNKS))
HOST_OBJS   := $(patsubst %.c,$(OBJ)/%.o,$(HOST_SRCS))
STUB_OBJS   := $(patsubst %.c,$(OBJ)/%.o,$(STUB_SRCS))
MOJO_OBJS   := $(OBJ)/mojoshader.o $(OBJ)/mojoshader_common.o \
               $(OBJ)/mojoshader_profile_common.o $(OBJ)/mojoshader_profile_metal.o

OBJS := $(CHUNK_OBJS) $(OBJ)/engine_bundle.o $(OBJ)/engine_imports.o \
        $(HOST_OBJS) $(STUB_OBJS) $(MOJO_OBJS)

.PHONY: all clean run aaudio-check
all: halo-headless

$(OBJ):
	mkdir -p $(OBJ)

# Generated engine chunks (in $(GEN)).
$(OBJ)/%.o: $(GEN)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Portable host shims (in $(SRC) = EngineHost).
$(OBJ)/%.o: $(SRC)/%.c $(VISION)/native/EngineReuse/engine_cpu.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# metalshader needs the Mojo version-include guard (matches macOS).
$(OBJ)/metalshader.o: $(SRC)/metalshader.c $(SRC)/metalshader.h | $(OBJ)
	$(CC) $(CFLAGS) -DMOJOSHADER_NO_VERSION_INCLUDE=1 -c $< -o $@

# Vendored MojoShader (C).
$(OBJ)/mojoshader.o: $(MOJODIR)/mojoshader.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJODEFS) -I$(MOJODIR) -c $< -o $@
$(OBJ)/mojoshader_common.o: $(MOJODIR)/mojoshader_common.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJODEFS) -I$(MOJODIR) -c $< -o $@
$(OBJ)/mojoshader_profile_common.o: $(MOJODIR)/profiles/mojoshader_profile_common.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJODEFS) -I$(MOJODIR) -c $< -o $@
$(OBJ)/mojoshader_profile_metal.o: $(MOJODIR)/profiles/mojoshader_profile_metal.c | $(OBJ)
	$(CC) -O2 -w -std=c11 -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJODEFS) -I$(MOJODIR) -c $< -o $@

# Headless stubs (in this directory).
$(OBJ)/%.o: %.c $(SRC)/metalrenderer.h $(SRC)/gamecontroller.h $(SRC)/metalwin.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Pin main.o to the local headless main.c: the host-shim pattern rule
# $(OBJ)/%.o: $(SRC)/%.c would otherwise match ../main.c (macOS, newer mtime)
# and pull in CoreFoundation. Explicit rules win over pattern rules.
$(OBJ)/main.o: main.c $(SRC)/host.h | $(OBJ)
	$(CC) $(CFLAGS) -c main.c -o $@

# Static variant: no PT_INTERP, no NEEDED libs, so qemu-aarch64 user-mode
# runs it directly without an Android rootfs (the NDK sysroot ships link-time
# stubs, not the runtime bionic linker64/libc.so/libm.so/libdl.so). Used to
# smoke-run the CPU translation loop on the build host.
halo-headless-static: $(OBJS)
	$(CC) -O2 -no-pie -static -o $@ $(OBJS) -lm
	@file $@

halo-headless: $(OBJS)
	$(CC) -O2 -o $@ $(OBJS) -lm -latomic
	@file $@
	@echo "OK: arm64-v8a headless binary built."

include jniLibs.mk

# Smoke on the host if qemu-aarch64 is installed; skipped otherwise.
run: halo-headless
	@if command -v qemu-aarch64 >/dev/null 2>&1; then \
	  QEMU_LD_PREFIX=$$(cd $(NDK)/toolchains/llvm/prebuilt/darwin-x86_64/sysroot && pwd) \
	  qemu-aarch64 ./halo-headless $(VISION)/game --frames 1; \
	else \
	  echo "qemu-aarch64 not installed; skipping run (binary built for arm64-v8a, needs a device/emulator)."; \
	fi

clean:
	rm -rf $(OBJ) halo-headless

# Vulkan renderer build: same chunks+host, replaces metalrenderer_stub.c
# with vulkanrenderer.c, links -lvulkan. Validates G1-G5 in lavapipe.
VK_OBJS := $(filter-out $(OBJ)/metalrenderer_stub.o,$(OBJS))
halo-headless-vk: $(VK_OBJS) $(OBJ)/vulkanrenderer.o | $(OBJ)
	$(CC) -O2 -o $@ $(VK_OBJS) $(OBJ)/vulkanrenderer.o -lm -lvulkan
	@file $@
	@echo "OK: halo-headless-vk (Vulkan renderer, arm64-v8a)."
$(OBJ)/vulkanrenderer.o: $(SRC)/vulkanrenderer.c $(SRC)/metalrenderer.h $(SRC)/vulkan_shaders.h | $(OBJ)
	$(CC) $(CFLAGS) -I$(VISION)/third_party/mojoshader -c $< -o $@

.PHONY: vk
vk: halo-headless-vk

# Compile-only check of the AAudio device sink (Phase 5 audio). The full
# device build swaps $(OBJ)/directsound_stub.o for $(OBJ)/aaudio_output.o
# and adds -lAAudio; this target proves the sink compiles for arm64-v8a
# against the NDK sysroot (static-asserts the AAudio constant mapping).
AAUDIO_OBJ := $(OBJ)/aaudio_output.o
aaudio-check: $(AAUDIO_OBJ)
	@echo "OK: aaudio_output.c compiles for arm64-v8a (AAudio sink, links -lAAudio)."
$(AAUDIO_OBJ): aaudio_output.c aaudio_output.h fake_audioqueue.h | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile-only check of the OpenXR device input bridge (Phase 5 input).
# The device build swaps $(OBJ)/gamecontroller_stub.o for
# $(OBJ)/openxr_input.o (defines HALO_OPENXR_INPUT, links -lopenxr); this
# target proves the bridge compiles for arm64-v8a against the vendored
# OpenXR 1.0 headers.
XR_INPUT_OBJ := $(OBJ)/openxr_input.o
.PHONY: xr-input-check
xr-input-check: $(XR_INPUT_OBJ)
	@echo "OK: openxr_input.c compiles for arm64-v8a (OpenXR actions, links -lopenxr)."
$(XR_INPUT_OBJ): openxr_input.c openxr_input.h $(SRC)/gamecontroller.h $(VISION)/third_party/openxr/include/openxr/openxr.h | $(OBJ)
	$(CC) $(CFLAGS) -DHALO_OPENXR_INPUT -I$(VISION)/third_party/openxr/include -c $< -o $@
