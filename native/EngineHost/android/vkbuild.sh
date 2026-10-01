#!/bin/bash
# vkbuild.sh - container-native build of the full engine with the Vulkan renderer.
# Runs inside the halo-vk-dev container (glibc, gcc, libvulkan).
# Usage: docker run --rm --platform linux/arm64 -v "$PWD:/w" -w /w halo-vk-dev:latest bash /w/native/EngineHost/android/vkbuild.sh
set -e
cd /w
OBJ=native/build/vk-obj
mkdir -p $OBJ
SRC=native/EngineHost
GEN=native/build/engine-reuse/whole-exe
CFLAGS="-O2 -w -std=gnu11 -D__ANDROID__ -DHALO_NO_PTHREAD_CANCEL -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 -I$SRC -Inative/EngineReuse -I$GEN -I$SRC/android"
# chunks
for f in $GEN/chunk_*.c; do
  echo "CC $(basename $f)"
  gcc $CFLAGS -c $f -o $OBJ/$(basename $f .c).o &
done
wait
# engine bundle + imports
gcc $CFLAGS -c $GEN/engine_bundle.c -o $OBJ/engine_bundle.o
gcc $CFLAGS -c $GEN/engine_imports.c -o $OBJ/engine_imports.o
# host shims
for f in host.c threading.c haptics.c halo_settings.c pointer.c shims_kernel32.c shims_misc.c d3d9.c texture_decode.c metalshader.c dinput8.c ddraw.c directsound.c directsound_mixer.c vorbis_shim.c resources.c overrides.c; do
  echo "CC $f"
  gcc $CFLAGS -c $SRC/$f -o $OBJ/$(basename $f .c).o &
done
wait
# vulkan renderer
gcc $CFLAGS -c $SRC/vulkanrenderer.c -o $OBJ/vulkanrenderer.o
# android stubs
for f in metalwin_stub.c gamecontroller_stub.c directsound_stub.c main.c; do
  echo "CC $f"
  gcc $CFLAGS -c $SRC/android/$f -o $OBJ/$(basename $f .c).o &
done
wait

MOJO=third_party/mojoshader
MOJODEFS="-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_GLSL120=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_GLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0 -DSUPPORT_PROFILE_METAL=0 -DSUPPORT_PROFILE_SPIRV=1 -DSUPPORT_PROFILE_GLSPIRV=0"
for f in $MOJO/mojoshader.c $MOJO/mojoshader_common.c $MOJO/profiles/mojoshader_profile_common.c $MOJO/profiles/mojoshader_profile_metal.c $MOJO/profiles/mojoshader_profile_spirv.c; do
  echo "CC $(basename $f)"
  gcc -O2 -w -std=c11 -include alloca.h -DMOJOSHADER_NO_VERSION_INCLUDE=1 $MOJODEFS -I$MOJO -c $f -o $OBJ/$(basename $f .c).o &
done
wait
# link
echo "LINK"
gcc -O2 -o $OBJ/halo-headless-vk $OBJ/*.o -lm -lvulkan -lpthread
command -v file >/dev/null 2>&1 && file $OBJ/halo-headless-vk || true
echo "OK: halo-headless-vk built."
