# Headless Vulkan dev image (arm64). Compiles the Vulkan renderer natively
# against glibc + lavapipe so shader translation, pipelines, textures and
# framebuffer readback can be validated without a headset. The same renderer
# .c is cross-compiled with the NDK for the device.
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential pkg-config libvulkan1 libvulkan-dev mesa-vulkan-drivers \
      vulkan-tools vulkan-validationlayers ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# Force the software driver (no GPU in the container).
ENV VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json
ENV VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json
ENV VK_ICD_FILENAMES_SHOW_ALL=1
