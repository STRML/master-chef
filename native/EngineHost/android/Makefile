# Android (arm64-v8a) headless build of the translated Halo engine host.
#
# Compiles the same generated engine chunks + portable host shims as the
# macOS `halo-host`, replacing the three ObjC/Metal modules (metalwin.m,
# metalrenderer.m, gamecontroller.m) and the macOS main.c with headless
# stubs in this directory. The real directsound.c is kept; its AudioToolbox
# include is redirected to fake_audioqueue.h (see that file + directsound_stub.c).
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

.PHONY: all clean run
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

halo-headless: $(OBJS)
	$(CC) -O2 -o $@ $(OBJS) -lm -latomic
	@file $@
	@echo "OK: arm64-v8a headless binary built."

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
