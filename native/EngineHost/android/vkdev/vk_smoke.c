/* vk_smoke.c - minimal headless Vulkan round-trip, compiled and run natively
 * (glibc + lavapipe) inside the vkdev container.
 *
 * Proves the full loop that the Vulkan renderer will rely on:
 *   instance (validation on) -> device+queue -> image+memory (BGRA8,
 *   color-attachment|transfer-src) -> render pass + framebuffer ->
 *   begin(clear RED)/end -> submit -> waitIdle -> CmdCopyImageToBuffer ->
 *   map -> verify every pixel is RED -> write /tmp/vk_smoke.ppm.
 *
 * Exit 0 + "VK_SMOKE PASS" means the software Vulkan stack is a valid
 * development stand-in for the device GPU. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "VK FAIL %s -> %d (%s:%d)\n", #x, (int)r_, __FILE__, __LINE__); exit(1); } } while (0)

static VkPhysicalDevice pick_gpu(VkInstance inst) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (!n) { fprintf(stderr, "no Vulkan GPUs\n"); exit(1); }
    VkPhysicalDevice gpus[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(inst, &n, gpus);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        printf("gpu[%u] = %s (api %u.%u)\n", i, p.deviceName,
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion));
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) return gpus[i]; /* lavapipe */
    }
    return gpus[0];
}

int main(void) {
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    VkInstance inst = NULL;
    VK_CHECK(vkCreateInstance(&ici, NULL, &inst));

    VkPhysicalDevice gpu = pick_gpu(inst);
    uint32_t qf = 0, qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, NULL);
    int found = -1;
    for (uint32_t i = 0; i < qn; i++) {
        VkQueueFamilyProperties q[8];
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, q);
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { found = (int)i; break; }
    }
    if (found < 0) { fprintf(stderr, "no graphics queue\n"); return 1; }
    qf = (uint32_t)found;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    VkDevice dev = NULL;
    VK_CHECK(vkCreateDevice(gpu, &dci, NULL, &dev));
    VkQueue queue = NULL;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    /* color image */
    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_B8G8R8A8_UNORM;
    imi.extent = (VkExtent3D){ W, H, 1 };
    imi.mipLevels = 1; imi.arrayLayers = 1;
    imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = NULL;
    VK_CHECK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t memType = 0xFFFFFFFF;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memType = i; break; }
    if (memType == 0xFFFFFFFF) memType = 0;
    VkDeviceMemory mem = NULL;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memType;
    VK_CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    VK_CHECK(vkBindImageMemory(dev, img, mem, 0));

    /* staging buffer */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)W * H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer stage = NULL;
    VK_CHECK(vkCreateBuffer(dev, &bci, NULL, &stage));
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, stage, &br);
    uint32_t hostType = 0xFFFFFFFF;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((br.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { hostType = i; break; }
    VkDeviceMemory stageMem = NULL;
    mai.allocationSize = br.size; mai.memoryTypeIndex = hostType;
    VK_CHECK(vkAllocateMemory(dev, &mai, NULL, &stageMem));
    VK_CHECK(vkBindBufferMemory(dev, stage, stageMem, 0));

    /* render pass + framebuffer */
    VkAttachmentDescription att = {0};
    att.format = VK_FORMAT_B8G8R8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VkRenderPass pass = NULL;
    VK_CHECK(vkCreateRenderPass(dev, &rpi, NULL, &pass));
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivi.image = img; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = NULL;
    VK_CHECK(vkCreateImageView(dev, &ivi, NULL, &view));
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = pass; fbi.attachmentCount = 1; fbi.pAttachments = &view;
    fbi.width = W; fbi.height = H; fbi.layers = 1;
    VkFramebuffer fb = NULL;
    VK_CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

    /* record + submit */
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = qf;
    VkCommandPool pool = NULL;
    VK_CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmdb = NULL;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cai, &cmdb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VK_CHECK(vkBeginCommandBuffer(cmdb, &bi));
    VkClearValue clear;
    clear.color = (VkClearColorValue){ .float32 = { 1.0f, 0.0f, 0.0f, 1.0f } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = pass; rbi.framebuffer = fb;
    rbi.renderArea = (VkRect2D){ {0,0}, {W,H} };
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmdb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmdb);
    VkImageMemoryBarrier toCopy = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toCopy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopy.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopy.image = img;
    toCopy.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmdb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toCopy);
    VkBufferImageCopy bic = {0};
    bic.imageExtent = (VkExtent3D){ W, H, 1 };
    bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    vkCmdCopyImageToBuffer(cmdb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, 1, &bic);
    VK_CHECK(vkEndCommandBuffer(cmdb));

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmdb;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue));

    void *px = NULL;
    VK_CHECK(vkMapMemory(dev, stageMem, 0, (VkDeviceSize)W * H * 4, 0, &px));
    int bad = 0;
    uint8_t *b = (uint8_t *)px;
    for (int i = 0; i < W * H; i++) {
        if (b[i*4+0] < 200 && b[i*4+1] > 50) bad++; /* BGRA: B low, G low, R high */
        if (b[i*4+2] < 200) bad++;
    }
    FILE *fp = fopen("/tmp/vk_smoke.ppm", "wb");
    if (fp) { fprintf(fp, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { uint8_t rgb[3] = { b[i*4+2], b[i*4+1], b[i*4+0] }; fwrite(rgb, 1, 3, fp); }
        fclose(fp); }
    vkUnmapMemory(dev, stageMem);
    printf("first pixel BGRA = %d %d %d %d; bad=%d\n", b[0], b[1], b[2], b[3], bad);
    if (bad == 0) { printf("VK_SMOKE PASS\n"); return 0; }
    printf("VK_SMOKE FAIL\n");
    return 1;
}
