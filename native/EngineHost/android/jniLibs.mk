# jniLibs.mk - PIC object build for the APK's native library.
#
# The engine chunks are compiled non-PIC for the static executable (fastest);
# the APK's shared library needs the same sources compiled with -fPIC. This
# include keeps the rules in one file so the main Makefile stays untouched.
#
# Included by android/Makefile (add: include jniLibs.mk).

PIC_OBJ := $(OBJ)/pic
PIC_CHUNK_OBJS := $(patsubst $(GEN)/%.c,$(PIC_OBJ)/%.o,$(CHUNKS))
PIC_HOST_OBJS := $(patsubst %.c,$(PIC_OBJ)/%.o,$(HOST_SRCS))
PIC_STUB_OBJS := $(patsubst %.c,$(PIC_OBJ)/%.o,$(STUB_SRCS))
# MojoShader profile for the device: vkshader.c translates the game's D3D8
# tokens to SPIR-V, so the PIC MojoShader build flips SPIRV on and metal off
# (the metal profile never runs on Android; metalshader.o still compiles
# against the public API and its translation calls return "no replacement",
# mirroring the container vkbuild.sh single-SPIRV-set device build).
MOJOSPIRVDEFS := $(MOJODEFS) -DSUPPORT_PROFILE_METAL=0
MOJOSPIRVDEFS := $(subst SUPPORT_PROFILE_SPIRV=0,SUPPORT_PROFILE_SPIRV=1,$(MOJOSPIRVDEFS))
PIC_MOJO_OBJS := $(PIC_OBJ)/mojoshader.o $(PIC_OBJ)/mojoshader_common.o \
                 $(PIC_OBJ)/mojoshader_profile_common.o $(PIC_OBJ)/mojoshader_profile_spirv.o

$(PIC_OBJ):
	mkdir -p $(PIC_OBJ)

$(PIC_CHUNK_OBJS): $(PIC_OBJ)/%.o: $(GEN)/%.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(PIC_HOST_OBJS): $(PIC_OBJ)/%.o: $(SRC)/%.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 -c $< -o $@

$(PIC_OBJ)/metalshader.o: $(SRC)/metalshader.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 -c $< -o $@


$(PIC_STUB_OBJS): $(PIC_OBJ)/%.o: %.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@


$(PIC_OBJ)/mojoshader.o: $(MOJODIR)/mojoshader.c | $(PIC_OBJ)
	$(CC) -O2 -w -std=c11 -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJOSPIRVDEFS) -I$(MOJODIR) -c $< -o $@
$(PIC_OBJ)/mojoshader_common.o: $(MOJODIR)/mojoshader_common.c | $(PIC_OBJ)
	$(CC) -O2 -w -std=c11 -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJOSPIRVDEFS) -I$(MOJODIR) -c $< -o $@
$(PIC_OBJ)/mojoshader_profile_common.o: $(MOJODIR)/profiles/mojoshader_profile_common.c | $(PIC_OBJ)
	$(CC) -O2 -w -std=c11 -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJOSPIRVDEFS) -I$(MOJODIR) -c $< -o $@
$(PIC_OBJ)/mojoshader_profile_spirv.o: $(MOJODIR)/profiles/mojoshader_profile_spirv.c | $(PIC_OBJ)
	$(CC) -O2 -w -std=c11 -fPIC -DMOJOSHADER_NO_VERSION_INCLUDE=1 $(MOJOSPIRVDEFS) -I$(MOJODIR) -c $< -o $@
# Phase 4/6: the APK's native library with the NativeActivity entry.
# The XR shell (xr/) supplies android_main, the OpenXR session +
# per-eye swapchains, and the metalwin_present bridge (which replaces
# metalwin_stub.o in this link); the NDK's native_app_glue relays the
# activity callbacks. The real Vulkan renderer (vulkanrenderer.c)
# replaces the headless metalrenderer_stub.c, and links -lvulkan.
JNI_DIR := $(VISION)/native/build/android-arm64/apk/jniLibs/arm64-v8a
GLUE := $(NDK)/sources/android/native_app_glue
XR_SRCS := xr_shell.c xr_quads.c xr_lifecycle.c android_main.c
PIC_XR_OBJS := $(patsubst %.c,$(PIC_OBJ)/%.o,$(XR_SRCS))
PIC_GLUE_OBJ := $(PIC_OBJ)/android_native_app_glue.o
# Touch controllers: the OpenXR action bridge (openxr_input.c) replaces
# the macOS gamecontroller stub in this link (it defines hostgc_* and
# xr_input_attach/poll, which android_main.c calls).
PIC_XRINPUT_OBJ := $(PIC_OBJ)/openxr_input.o

$(PIC_XRINPUT_OBJ): openxr_input.c openxr_input.h | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -DHALO_OPENXR_INPUT \
	    -I$(VISION)/third_party/openxr/include -c $< -o $@
$(PIC_XR_OBJS): $(PIC_OBJ)/%.o: xr/%.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -DXR_USE_PLATFORM_ANDROID=1 -DXR_USE_GRAPHICS_API_VULKAN=1 \
	    -I. -Ixr -I$(GLUE) -I$(VISION)/third_party/openxr/include -c $< -o $@

$(PIC_GLUE_OBJ): $(GLUE)/android_native_app_glue.c | $(PIC_OBJ)
	$(CC) -O2 -w -fPIC -I$(GLUE) -c $< -o $@

# Real Vulkan renderer: replaces the headless stub's mr_* (which only
# counts uploads) with the lavapipe-validated vulkanrenderer.c. Linked
# -lvulkan; the XR shell's per-eye mr_create runs on this path.
PIC_VK_OBJ := $(PIC_OBJ)/vulkanrenderer.o
# vkshader: the D3D8-token -> SPIR-V translation layer (self-contained TU;
# mirrors the container vkbuild.sh host-shim compile).
PIC_VKSHADER_OBJ := $(PIC_OBJ)/vkshader.o
$(PIC_VKSHADER_OBJ): $(SRC)/vkshader.c $(SRC)/vkshader.h | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -I$(SRC) -c $< -o $@

$(PIC_VK_OBJ): $(SRC)/vulkanrenderer.c $(SRC)/metalrenderer.h $(SRC)/vulkan_shaders.h | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -I$(SRC) -c $< -o $@

jniLibs: $(PIC_CHUNK_OBJS) $(PIC_HOST_OBJS) $(PIC_STUB_OBJS) $(PIC_MOJO_OBJS) \
         $(PIC_XR_OBJS) $(PIC_GLUE_OBJ) $(PIC_XRINPUT_OBJ) $(PIC_VK_OBJ) $(PIC_VKSHADER_OBJ) \
         $(PIC_OBJ)/engine_bundle.o $(PIC_OBJ)/engine_imports.o
	mkdir -p $(JNI_DIR)
	$(CC) -O2 -shared -o $(JNI_DIR)/libhaloquest.so \
	    $(filter-out $(PIC_OBJ)/metalwin_stub.o $(PIC_OBJ)/gamecontroller_stub.o \
	                 $(PIC_OBJ)/metalrenderer_stub.o,$^) \
	    -lm -ldl -landroid -llog -lvulkan
	@nm -D $(JNI_DIR)/libhaloquest.so | grep android_main
	@file $(JNI_DIR)/libhaloquest.so

$(PIC_OBJ)/engine_bundle.o: $(GEN)/engine_bundle.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(PIC_OBJ)/engine_imports.o: $(GEN)/engine_imports.c | $(PIC_OBJ)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
