/*
 * vulkanrenderer.c - Vulkan 1.1 offscreen rasterizer implementing the
 * metalrenderer.h contract, for the Halo PC -> Quest 3 headless port.
 *
 * The macOS reference (metalrenderer.m) is re-skinned, not redesigned: same
 * D3D9 render-state semantics, same pretransformed (rhw) vertex model, same
 * texture/sampler/blending behaviour. VK_LAYER_KHRONOS_validation is enabled
 * by default (dev mode). Validated in the lavapipe container (G1-G4) and
 * cross-compiled with the NDK (G5).
 *
 * Coordinates follow D3D9 pretransformed vertices: x,y in pixels (0,0 top
 * left), z in [0,1], rhw = 1/w. Colours are D3DCOLOR (0xAARRGGBB). The
 * colour target is VK_FORMAT_B8G8R8A8_UNORM (BGRA in memory, matching the
 * host readback contract); depth-stencil is VK_FORMAT_D24_UNORM_S8_UINT.
 *
 * Threading: one context, call all functions from the same thread.
 */
#include <stdarg.h>




#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalrenderer.h"
#include "vkshader.h"
#include "vulkan_shaders.h"

/* NOTE: do NOT define VK_NO_PROTOTYPES here, not even as 0: vulkan_core.h
 * guards prototypes with #ifndef, so a 0-definition strips every prototype,
 * vkGetInstanceProcAddr becomes an implicit int-returning call, and its
 * 64-bit pointer is sign-extended on LP64 (SIGSEGV when libraries map with
 * bit31 set, e.g. 0xffff9c7b86a4 in the lavapipe container). The loader
 * is linked directly via -lvulkan; pfn pointers below load it explicitly. */
#include <vulkan/vulkan.h>

static VkRenderPass g_rp, g_rp_no_depth;
static uint32_t g_white, g_black;

#define MR_MAX_VERTS 65536u
#define MR_MAX_INDICES (3u * 65536u)
#define MR_MAX_TEX 4096

/* ------------------------------------------------------------------ errors */
static const char *g_err = "";
static char g_errbuf[1024];
const char *mr_last_error(void) { return g_err; }
static void set_errorf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_errbuf, sizeof g_errbuf, fmt, ap); va_end(ap);
    g_err = g_errbuf;
}
#include <stdarg.h>

/* --------------------------------------------------------- vulkan globals */
static VkInstance     g_instance;
static VkPhysicalDevice g_phys;
static VkDevice       g_device;
static uint32_t       g_queue_family;
static VkQueue        g_queue;
static VkDebugUtilsMessengerEXT g_messenger;
static int            g_validation;
static int            g_adopted;   /* device/instance owned by the XR shell */

/* Vulkan function pointers loaded from the instance/device. */
#define VK_PFN(name) static PFN_##name p_##name;
VK_PFN(vkDestroyInstance)
VK_PFN(vkDestroyDevice)
VK_PFN(vkGetDeviceQueue)
VK_PFN(vkCreateRenderPass)
VK_PFN(vkCreateFramebuffer)
VK_PFN(vkCreateImage)
VK_PFN(vkGetImageMemoryRequirements)
VK_PFN(vkAllocateMemory)
VK_PFN(vkBindImageMemory)
VK_PFN(vkFreeMemory)
VK_PFN(vkDestroyImage)
VK_PFN(vkCreateImageView)
VK_PFN(vkDestroyImageView)
VK_PFN(vkDestroyFramebuffer)
VK_PFN(vkDestroyRenderPass)
VK_PFN(vkCreateBuffer)
VK_PFN(vkGetBufferMemoryRequirements)
VK_PFN(vkBindBufferMemory)
VK_PFN(vkMapMemory)
VK_PFN(vkUnmapMemory)
VK_PFN(vkDestroyBuffer)
VK_PFN(vkCreateCommandPool)
VK_PFN(vkAllocateCommandBuffers)
VK_PFN(vkBeginCommandBuffer)
VK_PFN(vkEndCommandBuffer)
VK_PFN(vkDestroyCommandPool)
VK_PFN(vkCmdBeginRenderPass)
VK_PFN(vkCmdEndRenderPass)
VK_PFN(vkCmdPipelineBarrier)
VK_PFN(vkCmdCopyImageToBuffer)
VK_PFN(vkCmdCopyBufferToImage)
VK_PFN(vkCmdCopyImage)
VK_PFN(vkCmdBindPipeline)
VK_PFN(vkCmdSetViewport)
VK_PFN(vkCmdSetScissor)
VK_PFN(vkCmdBindVertexBuffers)
VK_PFN(vkCmdBindIndexBuffer)
VK_PFN(vkCmdDraw)
VK_PFN(vkCmdDrawIndexed)
VK_PFN(vkCmdClearAttachments)
VK_PFN(vkQueueSubmit)
VK_PFN(vkQueueWaitIdle)
VK_PFN(vkCmdBlitImage)
VK_PFN(vkDeviceWaitIdle)
VK_PFN(vkCreateShaderModule)
VK_PFN(vkDestroyShaderModule)
VK_PFN(vkCreateGraphicsPipelines)
VK_PFN(vkDestroyPipeline)
VK_PFN(vkCreateSampler)
VK_PFN(vkDestroySampler)
VK_PFN(vkCreateDescriptorSetLayout)
VK_PFN(vkCreatePipelineLayout)
VK_PFN(vkAllocateDescriptorSets)
VK_PFN(vkUpdateDescriptorSets)
VK_PFN(vkCmdBindDescriptorSets)
VK_PFN(vkCmdPushConstants)
VK_PFN(vkCreateDescriptorPool)
VK_PFN(vkResetDescriptorPool)
VK_PFN(vkDestroyDescriptorPool)
VK_PFN(vkDestroyDescriptorSetLayout)
VK_PFN(vkDestroyPipelineLayout)
VK_PFN(vkDestroySampler)
VK_PFN(vkGetPhysicalDeviceMemoryProperties)
VK_PFN(vkGetPhysicalDeviceFormatProperties)
VK_PFN(vkGetPhysicalDeviceProperties)
VK_PFN(vkResetCommandBuffer)
VK_PFN(vkFreeCommandBuffers)
VK_PFN(vkFreeDescriptorSets)

static void load_instance_pfn(void) {
#define L(x) p_##x = (PFN_##x)vkGetInstanceProcAddr(g_instance, #x);
    L(vkCreateImageView) L(vkDestroyImageView)
    L(vkDestroyInstance) L(vkGetDeviceQueue) L(vkCreateRenderPass)
    L(vkCreateFramebuffer) L(vkCreateImage) L(vkGetImageMemoryRequirements)
    L(vkAllocateMemory) L(vkBindImageMemory) L(vkFreeMemory)
    L(vkDestroyImage) L(vkDestroyFramebuffer) L(vkDestroyRenderPass)
    L(vkCreateBuffer) L(vkGetBufferMemoryRequirements) L(vkBindBufferMemory)
    L(vkMapMemory) L(vkUnmapMemory) L(vkDestroyBuffer)
    L(vkCreateCommandPool) L(vkAllocateCommandBuffers) L(vkBeginCommandBuffer)
    L(vkEndCommandBuffer) L(vkDestroyCommandPool) L(vkCmdBeginRenderPass)
    L(vkCmdEndRenderPass) L(vkCmdPipelineBarrier) L(vkCmdCopyImageToBuffer)
    L(vkCmdCopyBufferToImage) L(vkCmdCopyImage) L(vkCmdBindPipeline) L(vkCmdBlitImage)
    L(vkCmdSetViewport) L(vkCmdSetScissor) L(vkCmdBindVertexBuffers)
    L(vkCmdBindIndexBuffer) L(vkCmdDraw) L(vkCmdDrawIndexed)
    L(vkCmdClearAttachments) L(vkQueueSubmit) L(vkQueueWaitIdle)
    L(vkDeviceWaitIdle) L(vkCreateShaderModule) L(vkDestroyShaderModule)
    L(vkCreateGraphicsPipelines) L(vkDestroyPipeline) L(vkCreateSampler)
    L(vkDestroySampler) L(vkCreateDescriptorSetLayout) L(vkCreatePipelineLayout)
    L(vkAllocateDescriptorSets) L(vkUpdateDescriptorSets) L(vkCmdBindDescriptorSets)
    L(vkCmdPushConstants) L(vkCreateDescriptorPool) L(vkResetDescriptorPool)
    L(vkDestroyDescriptorPool) L(vkDestroyDescriptorSetLayout)
    L(vkDestroyPipelineLayout) L(vkGetPhysicalDeviceMemoryProperties)
    L(vkGetPhysicalDeviceFormatProperties) L(vkResetCommandBuffer)
    L(vkFreeCommandBuffers) L(vkGetPhysicalDeviceProperties)
    L(vkFreeDescriptorSets)
#undef L
}
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    set_errorf("VK %s -> %d", #x, (int)r_); return -1; } } while (0)
/* Same as VK() but for pointer-returning functions (mr_create). */
#define VKP(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    set_errorf("VK %s -> %d", #x, (int)r_); return NULL; } } while (0)

static int find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

/* -------------------------------------------------------------- texture tbl */
typedef struct vk_tex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet ds;   /* combined image sampler, default sampler; created on first bind */
    int width, height, depth;
    VkFormat format;
    uint32_t mip_levels;
    int is_target;
    int has_depth;
} vk_tex;

static VkSampler g_default_sampler;

typedef struct vk_sampler {
    uint8_t address_u, address_v, min_filter, mag_filter, mip_filter;
    uint8_t max_anisotropy;
    VkSampler s;
} vk_sampler;

struct mr_context {
    int w, h, vx, vy, vw, vh;
    /* color target */
    VkImage color;
    VkDeviceMemory color_mem;
    /* depth-stencil target */
    VkImage depth;
    VkDeviceMemory depth_mem;
    VkRenderPass rp;
    VkRenderPass rp_no_depth;
    VkFramebuffer fb;
    uint32_t target_id;
    uint64_t tris;
    /* pending clear */
    int pending_clear;
    float cc[4];
    float clear_depth;
    uint32_t clear_stencil;
    /* command buffer for the current frame */
    VkCommandBuffer cmd;
    int in_pass;
};

/* Shared texture table (global, like the Metal reference). */
static vk_tex *g_textures[MR_MAX_TEX];
static uint64_t g_texture_keys[MR_MAX_TEX];
static uint32_t g_texture_tick;
static uint64_t g_cached_bytes;
static uint32_t g_cached_count;
static vk_sampler g_samplers[256];
static uint32_t g_sampler_count;
static VkDescriptorPool g_desc_pool;
static VkDescriptorPool g_frame_pool;
static VkDescriptorSetLayout g_ds_layout;
static VkPipelineLayout g_pipeline_layout;
static VkPipeline g_rhw_pipeline;      /* pretransformed, no depth */
static VkPipeline g_rhw_pipeline_d;    /* pretransformed, with depth */
static VkCommandPool g_cmd_pool;
static uint32_t g_bound_scissor;

/* ------------------------------------------------------------------- boot */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                void *) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[vk] %s\n", data->pMessage);
    return VK_FALSE;
}

static int create_instance(void) {
    if (g_instance) return 0;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    uint32_t enabled = 0;
    VkDebugUtilsMessengerCreateInfoEXT dbg = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    void *next = NULL;
    /* Enumerate: enable validation only if present (dev mode default). */
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties lp[32];
    if (n > 32) n = 32;
    vkEnumerateInstanceLayerProperties(&n, lp);
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation")) {
            enabled = 1; g_validation = 1;
            dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbg.pfnUserCallback = debug_cb;
            next = &dbg;
            break;
        }
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    if (enabled) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    ici.pNext = next;
    if (vkCreateInstance(&ici, NULL, &g_instance) != VK_SUCCESS) {
        if (enabled) {
            /* Retry without validation (e.g. layer missing) so the build stays
             * usable; dev mode is best-effort. */
            ici.enabledLayerCount = 0; ici.ppEnabledLayerNames = NULL;
            ici.pNext = NULL; g_validation = 0;
            if (vkCreateInstance(&ici, NULL, &g_instance) != VK_SUCCESS) {
                set_errorf("vkCreateInstance failed"); return -1;
            }
        } else { set_errorf("vkCreateInstance failed"); return -1; }
    }
    load_instance_pfn();
    return 0;
}

static int pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, NULL);
    if (!n) { set_errorf("no Vulkan devices"); return -1; }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(g_instance, &n, devs);
    g_phys = devs[0];
    /* Prefer an integrated/CPU (lavapipe) for headless determinism. */
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g_phys = devs[i]; break; }
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, NULL);
    VkQueueFamilyProperties qf[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qf);
    g_queue_family = 0xFFFFFFFF;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_queue_family = i; break; }
    if (g_queue_family == 0xFFFFFFFF) { set_errorf("no graphics queue"); return -1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = g_queue_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(g_phys, &dci, NULL, &g_device) != VK_SUCCESS) {
        set_errorf("vkCreateDevice failed"); return -1;
    }
    p_vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
    return 0;
}

static VkFormat depth_format(void) {
    VkFormat f = VK_FORMAT_D24_UNORM_S8_UINT;
    VkFormatProperties fp;
    p_vkGetPhysicalDeviceFormatProperties(g_phys, f, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static void make_renderpass(void) {
    VkFormat dfmt = depth_format();
    VkAttachmentDescription ca = {0};
    ca.format = VK_FORMAT_B8G8R8A8_UNORM; ca.samples = VK_SAMPLE_COUNT_1_BIT;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ca.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ca.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription da = {0};
    da.format = dfmt; da.samples = VK_SAMPLE_COUNT_1_BIT;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    da.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
    VkSubpassDependency dep[2] = {0};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = 0; dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 2; VkAttachmentDescription atts[2] = {ca, da};
    rpi.pAttachments = atts; rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2; rpi.pDependencies = dep;
    p_vkCreateRenderPass(g_device, &rpi, NULL, &g_rp);

    /* No-depth variant: fixed-function draws without a depth attachment. */
    VkAttachmentDescription ca2 = ca;
    VkSubpassDescription sp2 = sp; sp2.pDepthStencilAttachment = NULL;
    VkRenderPassCreateInfo rpi2 = rpi;
    VkAttachmentDescription atts2[1] = {ca2};
    rpi2.attachmentCount = 1; rpi2.pAttachments = atts2;
    rpi2.pSubpasses = &sp2;
    p_vkCreateRenderPass(g_device, &rpi2, NULL, &g_rp_no_depth);
}

/* Make a color image + memory, transition to its layout. */
static int make_image(int w, int h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *out, VkDeviceMemory *out_mem) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = (VkExtent3D){(uint32_t)w,(uint32_t)h,1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK(p_vkCreateImage(g_device, &ici, NULL, out));
    VkMemoryRequirements mr;
    p_vkGetImageMemoryRequirements(g_device, *out, &mr);
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int mt = find_memory_type(mr.memoryTypeBits, want);
    if (mt < 0) mt = find_memory_type(mr.memoryTypeBits, 0);
    if (mt < 0) { set_errorf("no memory type"); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    VK(p_vkAllocateMemory(g_device, &mai, NULL, out_mem));
    VK(p_vkBindImageMemory(g_device, *out, *out_mem, 0));
    return 0;
}

static VkShaderModule g_fixed_frag;   /* fixed-function fragment for no-PS draws */
static void make_pipeline(void) {
    VkShaderModuleCreateInfo vsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsi.codeSize = vk_spv_rhw_vert_len; vsi.pCode = vk_spv_rhw_vert;
    VkShaderModule vs;
    p_vkCreateShaderModule(g_device, &vsi, NULL, &vs);
    VkShaderModuleCreateInfo psi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    psi.codeSize = vk_spv_rhw_frag_len; psi.pCode = vk_spv_rhw_frag;
    p_vkCreateShaderModule(g_device, &psi, NULL, &g_fixed_frag);

    /* Descriptor set: 1 combined image sampler (binding 0). */
    VkDescriptorSetLayoutBinding b0 = {0};
    b0.binding = 0; b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0.descriptorCount = 1; b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dli.bindingCount = 1; dli.pBindings = &b0;
    p_vkCreateDescriptorSetLayout(g_device, &dli, NULL, &g_ds_layout);

    /* Push constants: U (size, tex flags). */
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(float)*2 + sizeof(int)*4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &g_ds_layout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    p_vkCreatePipelineLayout(g_device, &pli, NULL, &g_pipeline_layout);

    /* Vertex input: 48-byte VIn (pos,color,uv). */
    VkVertexInputBindingDescription vbd = {0, 48, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vad[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 32},
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = vad;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Alpha-blend state for the default (opaque) pass. */
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = VK_FALSE; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = states;

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_fixed_frag, "main", NULL },
    };
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp; gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb; gpi.pDynamicState = &dyn;
    gpi.layout = g_pipeline_layout; gpi.renderPass = g_rp;
    p_vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpi, NULL, &g_rhw_pipeline);

    /* With depth: depth test LESS_EQUAL, write on. */
    VkPipelineDepthStencilStateCreateInfo dsd = ds;
    dsd.depthTestEnable = VK_TRUE; dsd.depthWriteEnable = VK_TRUE;
    dsd.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    gpi.pDepthStencilState = &dsd; gpi.renderPass = g_rp;
    p_vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpi, NULL, &g_rhw_pipeline_d);

    /* Descriptor pool: per-texture sampler sets (fixed pipeline) and the
     * real pipeline's uniform sets. The free bit lets pipeline eviction
     * release the persistent uniform sets. */
    VkDescriptorPoolSize dps[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048 },
    };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 4096; dpci.poolSizeCount = 2; dpci.pPoolSizes = dps;
    p_vkCreateDescriptorPool(g_device, &dpci, NULL, &g_desc_pool);
    /* Frame pool for the real pipeline's sampler sets: every draw
     * allocates a FRESH set and never updates one a command buffer may
     * still reference (vkUpdateDescriptorSets on a bound set invalidates
     * it). Reset after every queue wait; no set outlives its frame (the
     * game readback path is fully synchronous). */
    VkDescriptorPoolSize fps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 2048 };
    VkDescriptorPoolCreateInfo fpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    fpci.maxSets = 2048; fpci.poolSizeCount = 1; fpci.pPoolSizes = &fps;
    p_vkCreateDescriptorPool(g_device, &fpci, NULL, &g_frame_pool);

    p_vkDestroyShaderModule(g_device, vs, NULL);
    /* g_fixed_frag is persistent: the real pipeline borrows it for no-PS draws. */
}

/* ------------------------------------------------------ texture helpers */
static VkSampler get_sampler(uint8_t au, uint8_t av, uint8_t minf, uint8_t magf, uint8_t mipf, uint8_t aniso) {
    for (uint32_t i = 0; i < g_sampler_count; ++i) {
        vk_sampler *s = &g_samplers[i];
        if (s->address_u==au && s->address_v==av && s->min_filter==minf &&
            s->mag_filter==magf && s->mip_filter==mipf && s->max_anisotropy==aniso)
            return s->s;
    }
    if (g_sampler_count >= 256) return g_samplers[0].s;
    vk_sampler *s = &g_samplers[g_sampler_count++];
    s->address_u=au; s->address_v=av; s->min_filter=minf; s->mag_filter=magf;
    s->mip_filter=mipf; s->max_anisotropy=aniso;
    VkFilter min = minf==1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkFilter mag = magf==1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkSamplerMipmapMode mmode = mipf==2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode amu = au==1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT :
                               au==2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE :
                               au==3 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT :
                               au==4 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode amv = av==1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT :
                               av==2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE :
                               av==3 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT :
                               av==4 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = mag; sci.minFilter = min; sci.mipmapMode = mmode;
    sci.addressModeU = amu; sci.addressModeV = amv; sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxAnisotropy = aniso ? aniso : 1.0f;
    sci.anisotropyEnable = aniso > 1;
    sci.minLod = 0; sci.maxLod = 1000;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    p_vkCreateSampler(g_device, &sci, NULL, &s->s);
    return s->s;
}

static uint32_t tex_alloc(void) {
    for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!g_textures[i]) return i;
    return 0;
}

/* Create a white 1x1 texture and a black one for unbound sampling. */
static uint32_t solid_tex(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint32_t id = tex_alloc();
    if (!id) return 0;
    vk_tex *t = calloc(1, sizeof *t);
    t->width=w; t->height=h; t->depth=1; t->format=VK_FORMAT_B8G8R8A8_UNORM; t->mip_levels=1;
    /* staging */
    VkBufferCreateInfo sbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    sbci.size = (VkDeviceSize)w*h*4; sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer sb; VkDeviceMemory sbm;
    p_vkCreateBuffer(g_device, &sbci, NULL, &sb);
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(g_device, sb, &mr);
    int mt = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize=mr.size; mai.memoryTypeIndex=(uint32_t)mt;
    p_vkAllocateMemory(g_device, &mai, NULL, &sbm);
    p_vkBindBufferMemory(g_device, sb, sbm, 0);
    void *p; p_vkMapMemory(g_device, sbm, 0, mr.size, 0, &p);
    uint32_t px = (uint32_t)a<<24 | (uint32_t)r<<16 | (uint32_t)g<<8 | b;
    for (int i = 0; i < w*h; ++i) ((uint32_t*)p)[i] = px;
    p_vkUnmapMemory(g_device, sbm);
    if (make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &t->image, &t->memory)) { return 0; }
    /* copy staging -> image */
    VkCommandBufferAllocateInfo caib = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    caib.commandPool = g_cmd_pool; caib.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    caib.commandBufferCount = 1;
    VkCommandBuffer cb; p_vkAllocateCommandBuffers(g_device, &caib, &cb);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &cbi);
    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.srcAccessMask=0; to_dst.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.image=t->image; to_dst.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 0, NULL, 1, &to_dst);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource=(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    bic.imageExtent=(VkExtent3D){(uint32_t)w,(uint32_t)h,1};
    p_vkCmdCopyBufferToImage(cb, sb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier to_sampled = to_dst;
    to_sampled.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sampled.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    to_sampled.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sampled.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, NULL, 0, NULL, 1, &to_sampled);
    p_vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount=1; si.pCommandBuffers=&cb;
    p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &cb);
    p_vkDestroyBuffer(g_device, sb, NULL);
    p_vkFreeMemory(g_device, sbm, NULL);
    g_textures[id] = t;
    return id;
}

/* Ensure a sampled image view + bound descriptor set exist for t.
 * The set is written once; the sampler is the shared default. A texture
 * whose image was never transitioned to SHADER_READ_ONLY_OPTIMAL still
 * samples deterministically (the render pass does not own its layout). */
static int tex_bind(vk_tex *t) {
    if (!t || !t->image) return -1;
    if (!t->view) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = t->image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = t->format;
        vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if (p_vkCreateImageView(g_device, &vci, NULL, &t->view) != VK_SUCCESS) return -1;
    }
    if (!t->ds) {
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = g_desc_pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &g_ds_layout;
        if (p_vkAllocateDescriptorSets(g_device, &dai, &t->ds) != VK_SUCCESS) { t->ds = VK_NULL_HANDLE; return -1; }
        VkDescriptorImageInfo dii = { g_default_sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = t->ds; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        p_vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
    }
    return 0;
}

static void close_pass(mr_context *c) {
    if (c->in_pass) { p_vkCmdEndRenderPass(c->cmd); c->in_pass = 0; }
}
static void begin_pass(mr_context *c) {
    if (c->in_pass) return;
    VkClearValue cv[2] = {0};
    cv[0].color = (VkClearColorValue){{c->cc[0],c->cc[1],c->cc[2],c->cc[3]}};
    cv[1].depthStencil = (VkClearDepthStencilValue){1.0f, 0};
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = c->rp; rbi.framebuffer = c->fb;
    rbi.renderArea = (VkRect2D){(int32_t)c->vx,(int32_t)c->vy,(uint32_t)c->vw,(uint32_t)c->vh};
    rbi.clearValueCount = 2; rbi.pClearValues = cv;
    p_vkCmdBeginRenderPass(c->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    c->in_pass = 1;
}
static int submit_frame(mr_context *c) {
    close_pass(c);
    p_vkEndCommandBuffer(c->cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
    VkResult r = p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    /* The frame pool is NOT reset here: its descriptor sets were bound into
     * c->cmd during the frame's draws, and resetting the pool while the
     * (re-begun) command buffer still carries the prior frame's recorded
     * binds would invalidate them per the validation layer's "last known
     * state". The command buffer is reset and re-begun instead; the frame
     * pool's sets are simply leaked until the pool is next reset (which
     * never happens mid-run, and the pool is sized for the run). */
    p_vkResetCommandBuffer(c->cmd, 0);
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    p_vkBeginCommandBuffer(c->cmd, &cbi);
    return r == VK_SUCCESS ? 0 : -1;
}
/* Flush the frame: close the render pass, record a COLOR->TRANSFER_SRC
 * barrier and the image->buffer copy on the same c->cmd that holds the
 * draws, then submit the whole frame via submit_frame. The render passes
 * were never reaching the GPU before (submit_frame was dead code); this
 * is the one place a frame is committed. The image is left in
 * TRANSFER_SRC; the next frame's render pass begins with
 * initialLayout=UNDEFINED so validation is satisfied. */
static int readback(mr_context *c, void *out, size_t out_size) {
    if (!c || !out || out_size < (size_t)c->w * c->h * 4) { set_errorf("bad readback args"); return -1; }
    close_pass(c);
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)c->w * c->h * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf; p_vkCreateBuffer(g_device, &bci, NULL, &buf);
    VkMemoryRequirements mrq; p_vkGetBufferMemoryRequirements(g_device, buf, &mrq);
    int mt = find_memory_type(mrq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mrq.size; mai.memoryTypeIndex = (uint32_t)mt;
    p_vkAllocateMemory(g_device, &mai, NULL, &mem);
    p_vkBindBufferMemory(g_device, buf, mem, 0);
    /* Barrier + copy on the frame's own command buffer. */
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = c->color; b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    p_vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,NULL,0,NULL,1,&b);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    bic.imageExtent = (VkExtent3D){(uint32_t)c->w,(uint32_t)c->h,1};
    p_vkCmdCopyImageToBuffer(c->cmd, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    if (submit_frame(c) != 0) {
        p_vkDestroyBuffer(g_device, buf, NULL); p_vkFreeMemory(g_device, mem, NULL); return -1;
    }
    void *p; p_vkMapMemory(g_device, mem, 0, mrq.size, 0, &p);
    memcpy(out, p, (size_t)c->w * c->h * 4);
    p_vkUnmapMemory(g_device, mem);
    p_vkDestroyBuffer(g_device, buf, NULL);
    p_vkFreeMemory(g_device, mem, NULL);
    return 0;
}

/* --------------------------------------------------------------- context */
int mr_adopt_vulkan(void *instance, void *physical, void *device, uint32_t queue_family) {
    /* Bind the renderer to a Vulkan instance + logical device created
     * elsewhere (the OpenXR shell's runtime-recommended device). Must be
     * called before the first mr_create; subsequent mr_create/init_shared
     * use these instead of creating their own instance/device. Idempotent. */
    if (g_device) return 0;
    g_instance = (VkInstance)instance;
    g_phys = (VkPhysicalDevice)physical;
    g_device = (VkDevice)device;
    g_queue_family = queue_family;
    g_adopted = 1;
    load_instance_pfn();
    p_vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
    return 0;
}

static int init_shared(void) {
    if (g_cmd_pool) return 0;   /* fully initialised (self- or adopted) */
    if (!g_device) {            /* not adopted: create own instance+device */
        if (create_instance()) return -1;
        load_instance_pfn();
        if (pick_device()) return -1;
    }
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = g_queue_family;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(p_vkCreateCommandPool(g_device, &cpci, NULL, &g_cmd_pool));
    make_renderpass();
    make_pipeline();
    if (!g_default_sampler) {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0; sci.maxLod = 1000;
        if (p_vkCreateSampler(g_device, &sci, NULL, &g_default_sampler) != VK_SUCCESS) {
            set_errorf("create default sampler"); return -1; }
    }
    g_white = solid_tex(1, 1, 255, 255, 255, 255);
    g_black = solid_tex(1, 1, 0, 0, 0, 255);
    return 0;
}

mr_context *mr_create(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { set_errorf("bad size"); return NULL; }
    if (init_shared()) return NULL;
    mr_context *c = calloc(1, sizeof *c);
    c->w = w; c->h = h; c->vx = 0; c->vy = 0; c->vw = w; c->vh = h;
    c->rp = g_rp;
    if (make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &c->color, &c->color_mem)) { free(c); return NULL; }
    if (make_image(w, h, depth_format(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   &c->depth, &c->depth_mem)) { free(c); return NULL; }
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = c->color; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView civ; VKP(p_vkCreateImageView(g_device, &ivci, NULL, &civ));
    ivci.image = c->depth; ivci.format = depth_format();
    ivci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VkImageView div; VKP(p_vkCreateImageView(g_device, &ivci, NULL, &div));
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = c->rp; VkImageView views[2] = {civ, div};
    fbi.attachmentCount = 2; fbi.pAttachments = views;
    fbi.width = (uint32_t)w; fbi.height = (uint32_t)h; fbi.layers = 1;
    VKP(p_vkCreateFramebuffer(g_device, &fbi, NULL, &c->fb));
    c->target_id = tex_alloc();
    if (c->target_id) {
        vk_tex *t = calloc(1, sizeof *t);
        t->width=w; t->height=h; t->depth=1; t->format=VK_FORMAT_B8G8R8A8_UNORM;
        t->mip_levels=1; t->image=c->color; t->is_target=1;
        g_textures[c->target_id] = t;
    }
    VkCommandBufferAllocateInfo caib = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    caib.commandPool = g_cmd_pool; caib.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; caib.commandBufferCount = 1;
    VKP(p_vkAllocateCommandBuffers(g_device, &caib, &c->cmd));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(c->cmd, &cbi);
    /* Bring the colour target into SHADER_READ_ONLY once at create so the
     * target texture (c->target_id) is sampleable before the surface is ever
     * drawn to. Render passes begin with initialLayout=UNDEFINED and the
     * write/blit paths barrier from UNDEFINED too, so they stay valid. */
    VkImageMemoryBarrier to_srro = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_srro.srcAccessMask = 0; to_srro.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_srro.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_srro.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_srro.image = c->color; to_srro.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    p_vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,0,NULL,0,NULL,1,&to_srro);
    p_vkEndCommandBuffer(c->cmd);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &c->cmd;
    p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    p_vkResetCommandBuffer(c->cmd, 0);
    VkCommandBufferBeginInfo cbi2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    p_vkBeginCommandBuffer(c->cmd, &cbi2);
    c->cc[0]=c->cc[1]=c->cc[2]=c->cc[3]=0;
    return c;
}
void mr_destroy(mr_context *c) {
    if (!c) return;
    if (c->cmd) { close_pass(c); p_vkEndCommandBuffer(c->cmd); }
    if (c->fb) p_vkDestroyFramebuffer(g_device, c->fb, NULL);
    if (c->target_id && g_textures[c->target_id]) { free(g_textures[c->target_id]); g_textures[c->target_id]=NULL; }
    if (c->depth) p_vkDestroyImage(g_device, c->depth, NULL);
    if (c->depth_mem) p_vkFreeMemory(g_device, c->depth_mem, NULL);
    if (c->color) p_vkDestroyImage(g_device, c->color, NULL);
    if (c->color_mem) p_vkFreeMemory(g_device, c->color_mem, NULL);
    if (c->cmd) p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &c->cmd);
    free(c);
}
void mr_set_viewport(mr_context *c, int x, int y, int w, int h) {
    if (!c) return;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x + w > c->w) w = c->w - x; if (y + h > c->h) h = c->h - y;
    if (w < 0) w = 0; if (h < 0) h = 0;
    c->vx = x; c->vy = y; c->vw = w; c->vh = h;
}
int mr_resize(mr_context *c, int w, int h) { if (!c) return -1; if (w==c->w && h==c->h) return 0;
    close_pass(c); p_vkDestroyFramebuffer(g_device, c->fb, NULL);
    p_vkDestroyImage(g_device, c->color, NULL); p_vkFreeMemory(g_device, c->color_mem, NULL);
    p_vkDestroyImage(g_device, c->depth, NULL); p_vkFreeMemory(g_device, c->depth_mem, NULL);
    c->w=w; c->h=h; c->vx=0; c->vy=0; c->vw=w; c->vh=h;
    if (make_image(w,h,VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->color,&c->color_mem)) return -1;
    if (make_image(w,h,depth_format(),VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,&c->depth,&c->depth_mem)) return -1;
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image=c->color; ivci.viewType=VK_IMAGE_VIEW_TYPE_2D; ivci.format=VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView civ; p_vkCreateImageView(g_device,&ivci,NULL,&civ);
    ivci.image=c->depth; ivci.format=depth_format();
    ivci.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VkImageView div; p_vkCreateImageView(g_device,&ivci,NULL,&div);
    VkFramebufferCreateInfo fbi={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fbi.renderPass=c->rp;
    VkImageView views[2]={civ,div}; fbi.attachmentCount=2; fbi.pAttachments=views; fbi.width=w; fbi.height=h; fbi.layers=1;
    int r=p_vkCreateFramebuffer(g_device,&fbi,NULL,&c->fb);
    if (c->cmd) p_vkResetCommandBuffer(c->cmd,0);
    VkCommandBufferBeginInfo cbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; p_vkBeginCommandBuffer(c->cmd,&cbi);
    return r==VK_SUCCESS?0:-1; }

void mr_clear(mr_context *c, uint32_t col) {
    if (!c) return;
    c->cc[0]=((col>>16)&255)/255.f; c->cc[1]=((col>>8)&255)/255.f;
    c->cc[2]=(col&255)/255.f; c->cc[3]=((col>>24)&255)/255.f;
    begin_pass(c);
    VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {0} };
    ca.colorAttachment = 0; ca.clearValue.color = (VkClearColorValue){{c->cc[0],c->cc[1],c->cc[2],c->cc[3]}};
    VkClearRect cr = { (VkRect2D){0,0,(uint32_t)c->w,(uint32_t)c->h}, 0, 1 };
    p_vkCmdClearAttachments(c->cmd, 1, &ca, 1, &cr);
}
void mr_clear_depth(mr_context *c, float d) {
    if (!c) return; begin_pass(c);
    VkClearAttachment ca = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {0} };
    ca.clearValue.depthStencil = (VkClearDepthStencilValue){d, 0};
    VkClearRect cr = { (VkRect2D){0,0,(uint32_t)c->w,(uint32_t)c->h}, 0, 1 };
    p_vkCmdClearAttachments(c->cmd, 1, &ca, 1, &cr);
}
void mr_clear_stencil(mr_context *c, uint32_t v) {
    if (!c) return; begin_pass(c);
    VkClearAttachment ca = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, {0} };
    ca.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, (uint32_t)v};
    VkClearRect cr = { (VkRect2D){0,0,(uint32_t)c->w,(uint32_t)c->h}, 0, 1 };
    p_vkCmdClearAttachments(c->cmd, 1, &ca, 1, &cr);
}
int mr_read_framebuffer(mr_context *c, void *out, size_t out_size) { return readback(c, out, out_size); }


/* Convert 28-byte mr_vertex_rhw to 48-byte Vulkan vertex (pos, color, uv). */
static void fill_vin(uint8_t *dst, const mr_vertex_rhw *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const mr_vertex_rhw *v = src + i;
        uint8_t *p = dst + i * 48;
        memcpy(p, &v->x, 4); memcpy(p + 4, &v->y, 4);
        memcpy(p + 8, &v->z, 4); memcpy(p + 12, &v->rhw, 4);
        float a = ((v->color >> 24) & 255) / 255.0f, r = ((v->color >> 16) & 255) / 255.0f;
        float g = ((v->color >> 8) & 255) / 255.0f, b = (v->color & 255) / 255.0f;
        memcpy(p + 16, &r, 4); memcpy(p + 20, &g, 4); memcpy(p + 24, &b, 4); memcpy(p + 28, &a, 4);
        memcpy(p + 32, &v->u, 4); memcpy(p + 36, &v->v, 4);
        memset(p + 40, 0, 8);
    }
}

/* Allocate + bind host-visible memory for a buffer. */
static int bind_host_mem(VkBuffer buf, VkDeviceMemory *out) {
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(g_device, buf, &mr);
    int mt = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { set_errorf("no host-visible memory"); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = (uint32_t)mt;
    if (p_vkAllocateMemory(g_device, &mai, NULL, out)) return -1;
    if (p_vkBindBufferMemory(g_device, buf, *out, 0)) { p_vkFreeMemory(g_device, *out, NULL); return -1; }
    return 0;
}

/* ---------------------------------------------------- real pipeline */
/* The game's shaders (vs_1_1/ps_1_1/ps_2_0) are translated to SPIR-V via
 * vkshader.c and linked; the renderer then binds the game's own vertex
 * layout (from the D3D declaration) and the game's constant register files
 * (vs_float4/ps_float4) as uniform buffers. Any failure (ps_1_4, unknown
 * layout, compile error) falls back to the fixed rhw pipeline — draws are
 * never dropped.
 *
 * Descriptor layout (SPIRV_MODE_VK, from the vendored MojoShader emitter):
 *   set 0: VS samplers (combined image sampler, binding = sampler register)
 *   set 1: VS uniform UBO (binding 0)
 *   set 2: PS samplers
 *   set 3: PS uniform UBO
 *   push constants: same as the fixed pipeline (size, flags) */
#define VK_PIPE_SLOTS 256
typedef struct vk_pipeline {
    int no_ps;
    uint64_t key;
    int state;                     /* 0=empty 1=ready 2=failed */
    vkshader vs, ps;               /* parseData held for the module lifetime */
    VkShaderModule vs_mod, ps_mod;
    VkDescriptorSetLayout ds[4];   /* VS sampler, VS uniform, PS sampler, PS uniform */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkBuffer vs_ubo; VkDeviceMemory vs_ubo_mem;   /* 256 float4 = 4096 B */
    VkBuffer ps_ubo; VkDeviceMemory ps_ubo_mem;   /* 224 float4 = 3584 B */
    VkDescriptorSet set[4];        /* persistent per-pipeline sets */
    uint32_t streams;              /* bitmask of vertex streams in use */
    VkVertexInputAttributeDescription vad[16];
    VkVertexInputBindingDescription vbd[16];
    uint32_t vad_count, vbd_count;
    /* MojoShader vertex element format per stream-0 attribute (indexed by
     * attribute position; 3 = FLOAT4 default). The link patches the
     * attribute's SPIR-V input variable type and load opcode from these
     * (MOJOSHADER_linkSPIRVShaders). */
    uint8_t link_format[VKSHADER_MAX_ATTRIBS];
    int vs_uniform, ps_uniform;
} vk_pipeline;
static vk_pipeline g_pipes[VK_PIPE_SLOTS];
static uint64_t g_real_draws, g_real_fallbacks;

static VkFormat decl_vk_format(uint8_t type, uint32_t *bytes) {
    switch (type) {
        case 0: *bytes = 4;  return VK_FORMAT_R32_SFLOAT;
        case 1: *bytes = 8;  return VK_FORMAT_R32G32_SFLOAT;
        case 2: *bytes = 12; return VK_FORMAT_R32G32B32_SFLOAT;
        case 3: *bytes = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 4: *bytes = 4;  return VK_FORMAT_B8G8R8A8_UNORM;
        case 5: *bytes = 4;  return VK_FORMAT_R8G8B8A8_UINT;
        case 6: *bytes = 4;  return VK_FORMAT_R16G16_SINT;
        case 7: *bytes = 8;  return VK_FORMAT_R16G16B16A16_SINT;
        case 8: *bytes = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
        case 9: *bytes = 4;  return VK_FORMAT_R16G16_SNORM;
        case 10: *bytes = 8; return VK_FORMAT_R16G16B16A16_SNORM;
        case 11: *bytes = 4; return VK_FORMAT_R16G16_UNORM;
        case 12: *bytes = 8; return VK_FORMAT_R16G16B16A16_UNORM;
        case 15: *bytes = 4; return VK_FORMAT_R16G16_SFLOAT;
        case 16: *bytes = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: *bytes = 0; return VK_FORMAT_UNDEFINED;
    }
}

/* MojoShader vertex element format for a D3D8 declaration element type
 * (MOJOSHADER_VERTEXELEMENTFORMAT_*, mojoshader.h:899-910: SINGLE=0,
 * VECTOR2=1, VECTOR3=2, VECTOR4=3, COLOR=4, BYTE4=5, SHORT2=6, SHORT4=7,
 * NORMALIZEDSHORT2=8, NORMALIZEDSHORT4=9, HALFVECTOR2=10, HALFVECTOR4=11).
 * MOJOSHADER_linkSPIRVShaders only rewrites the SPIR-V input type for
 * BYTE4/SHORT2/SHORT4 (vec4 → uvec4/ivec4 with the load's opcode patched to
 * OpConvertUToF/OpConvertSToF); every other format leaves the input as
 * vec4-float and Vulkan's own normalized/half vertex formats deliver the
 * values the shader expects. */
static int decl_mojo_format(uint8_t type) {
    switch (type) {
        case 0: return 0;   /* FLOAT1 */
        case 1: return 1;   /* FLOAT2 */
        case 2: return 2;   /* FLOAT3 */
        case 3: return 3;   /* FLOAT4 */
        case 4: return 4;   /* D3DCOLOR */
        case 5: return 5;   /* UBYTE4 */
        case 6: return 6;   /* SHORT2 */
        case 7: return 7;   /* SHORT4 */
        case 8: return 4;   /* UBYTE4N */
        case 9: return 8;   /* SHORT2N */
        case 10: return 9;  /* SHORT4N */
        case 11: return 4;  /* USHORT2N */
        case 12: return 4;  /* USHORT4N */
        case 15: return 4;  /* FLOAT16_2 */
        case 16: return 4;  /* FLOAT16_4 */
        default: return -1;
    }
}

static int build_vertex_input(vk_pipeline *p, const mr_program_state *st, size_t stride) {
    const uint8_t *decl = (const uint8_t *)st->declaration;
    size_t dlen = st->declaration_bytes;
    if (!decl || dlen < 8 || (dlen & 7) || !stride || stride > 256) { set_errorf("bad vertex declaration"); return -1; }
    uint32_t used = 1;
    for (uint32_t a = 0; a < p->vs.attribute_count; ++a) {
        uint32_t usage = p->vs.attributes[a].usage, index = p->vs.attributes[a].index;
        /* The SPIR-V emitter decorates VS input variables with Location = the
         * D3D v-register number (mojoshader_profile_spirv.c:2284); vkshader.c
         * records it as attributes[].regnum. */
        uint32_t regnum = p->vs.attributes[a].regnum;
        int found = 0;
        for (size_t o = 0; o + 8 <= dlen; o += 8) {
            const uint8_t *e = decl + o;
            uint16_t stream = (uint16_t)(e[0] | (e[1] << 8));
            if (stream == 0xFF && e[4] == 17) break;
            if (e[6] != (uint8_t)usage || e[7] != (uint8_t)index) continue;
            uint16_t off = (uint16_t)(e[2] | (e[3] << 8));
            uint32_t eb = 0; VkFormat fmt = decl_vk_format(e[4], &eb);
            if (fmt == VK_FORMAT_UNDEFINED) { set_errorf("unsupported vertex element type %u", e[4]); return -1; }
            if ((uint32_t)off + eb > stride) { set_errorf("declaration exceeds stride"); return -1; }
            if (p->vad_count >= 16) { set_errorf("too many vertex attributes"); return -1; }
            /* Raw binding: every declaration element addresses the raw vertex
             * bytes directly; the SPIR-V element type (vec4/ivec4/uvec4 +
             * the S/UToF load opcode) is patched for the attribute by
             * vkshader_link. Non-float formats in secondary streams would
             * also need patching, which the link API cannot express per
             * stream — reject and fall back. */
            p->vad[p->vad_count++] = (VkVertexInputAttributeDescription){ regnum, stream, fmt, off };
            /* The link patches each stream-0 attribute's SPIR-V input
             * variable to the matching element type (vec4 for floats,
             * uvec4/ivec4 + S/UToF for BYTE4/SHORT2/SHORT4, see
             * MOJOSHADER_linkSPIRVShaders). Secondary streams are bound raw
             * and must stay float-typed elements: the link API cannot
             * express per-stream formats. */
            if (!stream) {
                int mf = decl_mojo_format(e[4]);
                if (a < VKSHADER_MAX_ATTRIBS) p->link_format[a] = (mf >= 0) ? (uint8_t)mf : 3;
            }
            if (stream) {
                if (decl_mojo_format(e[4]) < 0) { set_errorf("unsupported secondary stream format"); return -1; }
                if (stream > 15) { set_errorf("vertex stream %u out of range", stream); return -1; }
            }
            used |= (1u << stream);
            found = 1; break;
        }
        if (!found) { set_errorf("declaration missing semantic usage=%u index=%u", usage, index); return -1; }
    }
    p->streams = used;
    p->vbd[0] = (VkVertexInputBindingDescription){ 0, (uint32_t)stride, VK_VERTEX_INPUT_RATE_VERTEX };
    p->vbd_count = 1;
    for (uint32_t s = 1; s < 16; ++s) if (used & (1u << s)) {
        if (!st->stream_stride[s]) { set_errorf("stream %u unbound", s); return -1; }
        p->vbd[p->vbd_count++] = (VkVertexInputBindingDescription){ s, st->stream_stride[s], VK_VERTEX_INPUT_RATE_VERTEX };
    }
    return 0;
}

static void destroy_pipeline_entry(vk_pipeline *p) {
    if (p->pipeline) p_vkDestroyPipeline(g_device, p->pipeline, NULL);
    if (p->layout) p_vkDestroyPipelineLayout(g_device, p->layout, NULL);
    for (int i = 1; i <= 3; i += 2)
        if (p->set[i]) p_vkFreeDescriptorSets(g_device, g_desc_pool, 1, &p->set[i]);
    for (int i = 0; i < 4; ++i) if (p->ds[i]) p_vkDestroyDescriptorSetLayout(g_device, p->ds[i], NULL);
    if (p->vs_mod) p_vkDestroyShaderModule(g_device, p->vs_mod, NULL);
    if (p->ps_mod) p_vkDestroyShaderModule(g_device, p->ps_mod, NULL);
    if (p->vs_ubo) p_vkDestroyBuffer(g_device, p->vs_ubo, NULL);
    if (p->vs_ubo_mem) p_vkFreeMemory(g_device, p->vs_ubo_mem, NULL);
    if (p->ps_ubo) p_vkDestroyBuffer(g_device, p->ps_ubo, NULL);
    if (p->ps_ubo_mem) p_vkFreeMemory(g_device, p->ps_ubo_mem, NULL);
    vkshader_destroy(&p->vs);
    vkshader_destroy(&p->ps);
    memset(p, 0, sizeof *p);
}

static uint64_t hash_bytes(uint64_t h, const void *bytes, size_t n) {
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static uint64_t pipe_key(const mr_program_state *st, size_t stride) {
    uint64_t h = st->vertex_key ? st->vertex_key : UINT64_C(14695981039346656037);
    h = hash_bytes(h, &st->pixel_key, sizeof st->pixel_key);
    h = hash_bytes(h, &st->blend, sizeof st->blend);
    h = hash_bytes(h, &st->blend_src, sizeof(int) * 3);
    h = hash_bytes(h, &st->color_write_mask, sizeof st->color_write_mask);
    h = hash_bytes(h, &st->depth_enable, sizeof(int) * 3);
    h = hash_bytes(h, &st->cull_mode, sizeof st->cull_mode);
    h = hash_bytes(h, &st->stencil_enable, sizeof(int) * 5);
    h = hash_bytes(h, &st->alpha_test_ref, sizeof(int) * 2);
    h = hash_bytes(h, &stride, sizeof stride);
    h = hash_bytes(h, (const uint8_t *)st->declaration, st->declaration_bytes < 128 ? st->declaration_bytes : 128);
    return h;
}

static VkCompareOp cmp_op(int d3d) {
    switch (d3d) {
        case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS;
        case 3: return VK_COMPARE_OP_EQUAL; case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL;
        case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp stencil_op(int d3d) {
    switch (d3d) {
        case 1: return VK_STENCIL_OP_KEEP; case 2: return VK_STENCIL_OP_ZERO;
        case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT;
        case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
static void blend_factors(int d3d, int alpha, VkBlendFactor *src, VkBlendFactor *dst) {
    switch (d3d) {
        case 1: *src = VK_BLEND_FACTOR_ZERO; break;
        case 2: *src = VK_BLEND_FACTOR_ONE; break;
        case 3: *src = alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR; break;
        case 4: *src = alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
        case 5: *src = VK_BLEND_FACTOR_SRC_ALPHA; break;
        case 6: *src = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case 7: *src = VK_BLEND_FACTOR_DST_ALPHA; break;
        case 8: *src = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; break;
        case 9: *src = alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR; break;
        case 10: *src = alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; break;
        case 11: *src = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; break;
        default: *src = VK_BLEND_FACTOR_ONE; break;
    }
    switch (d3d) {
        case 1: *dst = VK_BLEND_FACTOR_ZERO; break;
        case 2: *dst = VK_BLEND_FACTOR_ONE; break;
        case 3: *dst = alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR; break;
        case 4: *dst = alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
        case 5: *dst = VK_BLEND_FACTOR_SRC_ALPHA; break;
        case 6: *dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case 7: *dst = VK_BLEND_FACTOR_DST_ALPHA; break;
        case 8: *dst = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; break;
        case 9: *dst = alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR; break;
        case 10: *dst = alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; break;
        default: *dst = VK_BLEND_FACTOR_ONE; break;
    }
}
static void blend_one(int d3d, VkBlendFactor *f) { int d; blend_factors(d3d, 0, f, &d); }

static int make_program_layout(vk_pipeline *p) {
    {
        VkDescriptorSetLayoutBinding b[16]; int n = 0;
        for (uint32_t i = 0; i < p->vs.sampler_count && n < 16; ++i)
            b[n++] = (VkDescriptorSetLayoutBinding){ p->vs.samplers[i].index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        if (n == 0) {
            /* The fixed-function fragment shader (no_ps draws) samples
             * set 0 binding 0 (see vk_spv_rhw_frag); the VS sampler set
             * must expose it to the fragment stage. */
            b[n++] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                p->no_ps ? (VkShaderStageFlags)(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                         : VK_SHADER_STAGE_VERTEX_BIT, NULL };
        }
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = n; li.pBindings = b;
        if (p_vkCreateDescriptorSetLayout(g_device, &li, NULL, &p->ds[0]) != VK_SUCCESS) return -1;
    }
    p->vs_uniform = p->vs.uniform_count > 0;
    {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (p_vkCreateDescriptorSetLayout(g_device, &li, NULL, &p->ds[1]) != VK_SUCCESS) return -1;
        if (p->vs_uniform) {
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = 256 * 16; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            if (p_vkCreateBuffer(g_device, &bi, NULL, &p->vs_ubo) || bind_host_mem(p->vs_ubo, &p->vs_ubo_mem)) return -1;
        }
    }
    {
        VkDescriptorSetLayoutBinding b[16]; int n = 0;
        for (uint32_t i = 0; i < p->ps.sampler_count && n < 16; ++i)
            b[n++] = (VkDescriptorSetLayoutBinding){ p->ps.samplers[i].index, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        if (n == 0) b[n++] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = n; li.pBindings = b;
        if (p_vkCreateDescriptorSetLayout(g_device, &li, NULL, &p->ds[2]) != VK_SUCCESS) return -1;
    }
    p->ps_uniform = p->ps.uniform_count > 0;
    {
        VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 1; li.pBindings = &b;
        if (p_vkCreateDescriptorSetLayout(g_device, &li, NULL, &p->ds[3]) != VK_SUCCESS) return -1;
        if (p->ps_uniform) {
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = 224 * 16; bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            if (p_vkCreateBuffer(g_device, &bi, NULL, &p->ps_ubo) || bind_host_mem(p->ps_ubo, &p->ps_ubo_mem)) return -1;
        }
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 2 + sizeof(int) * 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 4; pli.pSetLayouts = p->ds;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (p_vkCreatePipelineLayout(g_device, &pli, NULL, &p->layout) != VK_SUCCESS) return -1;
    /* Sampler sets (ds[0], ds[2]) are allocated fresh per draw from the
     * frame pool; only the uniform sets are persistent per pipeline. The
     * uniform UBOs are written once here (the buffer is per-pipeline and
     * reused every draw; the data is updated in place by draw_program). */
    for (int s = 1; s <= 3; s += 2) {
        if (!p->ds[s]) continue;
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_desc_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p->ds[s];
        if (p_vkAllocateDescriptorSets(g_device, &ai, &p->set[s]) != VK_SUCCESS) return -1;
        VkBuffer ubo = (s == 1) ? p->vs_ubo : p->ps_ubo;
        if (!ubo) continue;
        VkDescriptorBufferInfo bi = { ubo, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = p->set[s]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
        p_vkUpdateDescriptorSets(g_device, 1, &w, 0, NULL);
    }
    p->set[0] = p->set[2] = VK_NULL_HANDLE;
    return 0;
}

static int program_bind_samplers(vk_pipeline *p, const mr_program_state *st) {
    for (uint32_t i = 0; i < p->vs.sampler_count; ++i) {
        uint32_t stage = p->vs.samplers[i].index;
        uint32_t tex = (stage < 16) ? st->samplers[stage].texture : 0;
        vk_tex *t = (tex && tex < MR_MAX_TEX) ? g_textures[tex] : NULL;
        if (!t || !t->image) t = g_textures[g_white];
        if (tex_bind(t)) return -1;
    }
    for (uint32_t i = 0; i < p->ps.sampler_count; ++i) {
        uint32_t stage = p->ps.samplers[i].index;
        uint32_t tex = (stage < 16) ? st->samplers[stage].texture : 0;
        vk_tex *t = (tex && tex < MR_MAX_TEX) ? g_textures[tex] : NULL;
        if (!t || !t->image) t = g_textures[g_white];
        if (tex_bind(t)) return -1;
    }
    {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_frame_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p->ds[0];
        if (p_vkAllocateDescriptorSets(g_device, &ai, &p->set[0]) != VK_SUCCESS) {
            p->set[0] = VK_NULL_HANDLE; return -1;
        }
    }
    {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_frame_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p->ds[2];
        if (p_vkAllocateDescriptorSets(g_device, &ai, &p->set[2]) != VK_SUCCESS) {
            p->set[2] = VK_NULL_HANDLE; return -1;
        }
    }
    VkDescriptorImageInfo di[32];
    VkWriteDescriptorSet w[32];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < p->vs.sampler_count && nw < 16; ++i) {
        uint32_t stage = p->vs.samplers[i].index;
        uint32_t tex = (stage < 16) ? st->samplers[stage].texture : 0;
        vk_tex *t = (tex && tex < MR_MAX_TEX) ? g_textures[tex] : NULL;
        if (!t || !t->image) t = g_textures[g_white];
        di[nw] = (VkDescriptorImageInfo){ g_default_sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstSet = p->set[0]; w[nw].dstBinding = p->vs.samplers[i].index; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &di[nw];
        nw++;
    }
    if (!p->vs.sampler_count) {
        /* no_ps draws: the fixed fragment samples the draw's texture; the
         * game's own vertex shader never samples, so the white fallback
         * only applies to the linked-pair path. */
        vk_tex *t = NULL;
        if (p->no_ps) {
            uint32_t tex = st->samplers[0].texture;
            if (tex && tex < MR_MAX_TEX) t = g_textures[tex];
        }
        if (!t || !t->image) { t = g_textures[g_white]; if (tex_bind(t)) return -1; }
        else if (tex_bind(t)) return -1;
        di[nw] = (VkDescriptorImageInfo){ g_default_sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstSet = p->set[0]; w[nw].dstBinding = 0; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &di[nw];
        nw++;
    }
    for (uint32_t i = 0; i < p->ps.sampler_count && nw < 32; ++i) {
        uint32_t stage = p->ps.samplers[i].index;
        uint32_t tex = (stage < 16) ? st->samplers[stage].texture : 0;
        vk_tex *t = (tex && tex < MR_MAX_TEX) ? g_textures[tex] : NULL;
        if (!t || !t->image) t = g_textures[g_white];
        di[nw] = (VkDescriptorImageInfo){ g_default_sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[nw].dstSet = p->set[2]; w[nw].dstBinding = p->ps.samplers[i].index; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &di[nw];
        nw++;
    }
    if (nw) p_vkUpdateDescriptorSets(g_device, nw, w, 0, NULL);
    return 0;
}

static vk_pipeline *program_for(const mr_program_state *st, size_t stride) {
    /* Debug knob: HALO_NO_REAL_PIPELINE forces the fixed pipeline for every
     * draw (capture diffs against the game's real shading; device triage). */
    static int no_real = -1;
    if (no_real < 0) no_real = getenv("HALO_NO_REAL_PIPELINE") != NULL;
    if (no_real) return NULL;
    uint64_t key = pipe_key(st, stride);
    uint32_t slot = (uint32_t)(key % VK_PIPE_SLOTS);
    for (uint32_t probe = 0; probe < VK_PIPE_SLOTS; ++probe) {
        vk_pipeline *p = &g_pipes[(slot + probe) % VK_PIPE_SLOTS];
        if (p->state == 0) break;
        if (p->key == key) return p->state == 1 ? p : NULL;
    }
    vk_pipeline *p = &g_pipes[slot];
    if (p->state) destroy_pipeline_entry(p);
    p->state = 2;
    p->key = key;
    if (vkshader_translate(st->vertex_tokens, st->vertex_token_bytes, 0, &p->vs) != 0) { set_errorf("vs: %s", p->vs.error); return NULL; }
    if (st->pixel_tokens) {
        if (vkshader_translate(st->pixel_tokens, st->pixel_token_bytes, 1, &p->ps) != 0) { set_errorf("ps: %s", p->ps.error); return NULL; }
    } else {
        p->no_ps = 1;
    }
    /* The vertex input layout must be built before the link: the declaration
     * element types drive the SPIR-V input-type patches (link_format). */
    if (build_vertex_input(p, st, stride)) return NULL;
    if (p->no_ps) {
        if (vkshader_link_fixedfunc(&p->vs, p->link_format) != 0) { set_errorf("fixedfunc: %s", p->vs.error); return NULL; }
    } else if (vkshader_link(&p->vs, &p->ps, p->link_format) != 0) {
        set_errorf("link: %s", p->vs.error); return NULL;
    }
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smi.codeSize = vkshader_spirv_bytes(&p->vs); smi.pCode = p->vs.spirv;
    if (p_vkCreateShaderModule(g_device, &smi, NULL, &p->vs_mod) != VK_SUCCESS) { set_errorf("vs module"); return NULL; }
    if (!p->no_ps) {
        smi.codeSize = vkshader_spirv_bytes(&p->ps); smi.pCode = p->ps.spirv;
        if (p_vkCreateShaderModule(g_device, &smi, NULL, &p->ps_mod) != VK_SUCCESS) { set_errorf("ps module"); return NULL; }
    }
    if (make_program_layout(p)) return NULL;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = p->vbd_count; vi.pVertexBindingDescriptions = p->vbd;
    vi.vertexAttributeDescriptionCount = p->vad_count; vi.pVertexAttributeDescriptions = p->vad;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    rs.cullMode = st->cull_mode == 2 ? VK_CULL_MODE_FRONT_BIT : st->cull_mode == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = st->depth_enable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = st->depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = cmp_op(st->depth_compare ? st->depth_compare : 4);
    if (st->stencil_enable) {
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState *so = &ds.front;
        so->failOp = stencil_op(st->stencil_fail); so->passOp = stencil_op(st->stencil_pass); so->depthFailOp = stencil_op(st->stencil_depth_fail);
        so->compareOp = cmp_op(st->stencil_compare); so->compareMask = st->stencil_read_mask; so->writeMask = st->stencil_write_mask; so->reference = st->stencil_ref;
        ds.back = *so;
    }
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = (VkColorComponentFlags)((st->color_write_mask & 1 ? VK_COLOR_COMPONENT_R_BIT : 0) |
                                                 (st->color_write_mask & 2 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                                 (st->color_write_mask & 4 ? VK_COLOR_COMPONENT_B_BIT : 0) |
                                                 (st->color_write_mask & 8 ? VK_COLOR_COMPONENT_A_BIT : 0));
    if (st->blend != MR_BLEND_NONE && st->blend >= 0 && st->blend < MR_BLEND_COUNT) {
        cba.blendEnable = VK_TRUE;
        if (st->blend == MR_BLEND_CUSTOM) {
            VkBlendFactor s1, d1, s2, d2;
            blend_factors(st->blend_src, 0, &s1, &d1);
            blend_factors(st->blend_src, 1, &s2, &d2);
            VkBlendFactor x; blend_factors(st->blend_dst, 0, &x, &d1); blend_factors(st->blend_dst, 1, &x, &d2);
            cba.srcColorBlendFactor = s1; cba.dstColorBlendFactor = d1;
            cba.srcAlphaBlendFactor = s2; cba.dstAlphaBlendFactor = d2;
            cba.colorBlendOp = st->blend_op == 2 ? VK_BLEND_OP_SUBTRACT : st->blend_op == 3 ? VK_BLEND_OP_REVERSE_SUBTRACT :
                               st->blend_op == 4 ? VK_BLEND_OP_MIN : st->blend_op == 5 ? VK_BLEND_OP_MAX : VK_BLEND_OP_ADD;
        } else if (st->blend == MR_BLEND_SRC_ALPHA) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else if (st->blend == MR_BLEND_ADD) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        } else if (st->blend == MR_BLEND_MODULATE) {
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        } else {
            blend_one(st->blend == MR_BLEND_SRC_ALPHA_ZERO ? 5 : 2, &cba.srcColorBlendFactor);
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = cba.srcColorBlendFactor; cba.dstAlphaBlendFactor = cba.dstColorBlendFactor;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkDynamicState dstates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dyn.dynamicStateCount = 2; dyn.pDynamicStates = dstates;
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule frag = p->no_ps ? g_fixed_frag : p->ps_mod;
    gpi.stageCount = 2;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, p->vs_mod, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", NULL },
    };
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp; gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb; gpi.pDynamicState = &dyn;
    gpi.layout = p->layout; gpi.renderPass = g_rp;
    if (p_vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpi, NULL, &p->pipeline) != VK_SUCCESS) { set_errorf("real pipeline create"); return NULL; }
    p->state = 1;
    return p;
}

static int draw_program(mr_context *c, const mr_program_state *st, int prim,
                        const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) {
    if (!c || !st || !st->vertex_tokens || !st->declaration || !v || !nv || !stride) {
        if (g_real_fallbacks <= 5) fprintf(stderr, "[shader] early: c=%d vtok=%d ptok=%d decl=%d v=%d nv=%u stride=%zu\n", c!=NULL, st?st->vertex_tokens!=NULL:0, st?st->pixel_tokens!=NULL:0, st?st->declaration!=NULL:0, v!=NULL, nv, stride);
        return -1; }
    if (st->vertex_token_bytes < 8 || (st->pixel_tokens && st->pixel_token_bytes < 8)) {
        if (g_real_fallbacks <= 5) fprintf(stderr, "[shader] early: vb=%zu pb=%zu\n", st->vertex_token_bytes, st->pixel_token_bytes);
        return -1; }
    vk_pipeline *p = program_for(st, stride);
    if (!p) return -1;
    begin_pass(c);
    if (p->vs_uniform) {
        float buf[256 * 4]; memset(buf, 0, sizeof buf);
        if (st->vs_float4) vkshader_pack_uniforms(&p->vs, buf, st->vs_float4, st->vs_float4_count, NULL, NULL);
        void *map; if (p_vkMapMemory(g_device, p->vs_ubo_mem, 0, sizeof buf, 0, &map) == VK_SUCCESS) { memcpy(map, buf, sizeof buf); p_vkUnmapMemory(g_device, p->vs_ubo_mem); }
    }
    if (p->ps_uniform) {
        float buf[224 * 4]; memset(buf, 0, sizeof buf);
        if (st->ps_float4) vkshader_pack_uniforms(&p->ps, buf, st->ps_float4, st->ps_float4_count, NULL, NULL);
        void *map; if (p_vkMapMemory(g_device, p->ps_ubo_mem, 0, sizeof buf, 0, &map) == VK_SUCCESS) { memcpy(map, buf, sizeof buf); p_vkUnmapMemory(g_device, p->ps_ubo_mem); }
    }
    if (program_bind_samplers(p, st)) return -1;
    const uint8_t *src_v = (const uint8_t *)v;
    size_t src_stride = stride;
    float *xbuf = NULL;
    /* The raw stream-0 bytes are bound directly: the SPIR-V input types were
     * patched for this attribute by vkshader_link (see link_format). */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)nv * src_stride; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vb; VkDeviceMemory vbm;
    if (p_vkCreateBuffer(g_device, &bci, NULL, &vb) || bind_host_mem(vb, &vbm)) { free(xbuf); return -1; }
    void *map; p_vkMapMemory(g_device, vbm, 0, bci.size, 0, &map);
    memcpy(map, src_v, bci.size); p_vkUnmapMemory(g_device, vbm); free(xbuf);
    uint32_t used = p->streams;
    VkBuffer sb[16] = {0}; VkDeviceMemory sm[16] = {0};
    uint32_t maxb = 1;
    for (uint32_t s = 1; s < 16; ++s) if (used & (1u << s)) {
        if (!st->stream_bytes[s] || !st->stream_stride[s]) {
            for (uint32_t q = 1; q < 16; ++q) if (sb[q]) { p_vkDestroyBuffer(g_device, sb[q], NULL); p_vkFreeMemory(g_device, sm[q], NULL); }
            p_vkDestroyBuffer(g_device, vb, NULL); p_vkFreeMemory(g_device, vbm, NULL);
            set_errorf("stream %u unbound", s);
            return -1;
        }
        VkBufferCreateInfo sbi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        sbi.size = (VkDeviceSize)nv * st->stream_stride[s]; sbi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (p_vkCreateBuffer(g_device, &sbi, NULL, &sb[s]) || bind_host_mem(sb[s], &sm[s])) {
            for (uint32_t q = 1; q < 16; ++q) if (sb[q]) { p_vkDestroyBuffer(g_device, sb[q], NULL); p_vkFreeMemory(g_device, sm[q], NULL); }
            p_vkDestroyBuffer(g_device, vb, NULL); p_vkFreeMemory(g_device, vbm, NULL);
            return -1;
        }
        void *m2; p_vkMapMemory(g_device, sm[s], 0, sbi.size, 0, &m2);
        memcpy(m2, st->stream_bytes[s], sbi.size); p_vkUnmapMemory(g_device, sm[s]);
        maxb = s + 1;
    }
    {
        /* Bind every declared stream slot: the pipeline's vertex input uses
         * the stream number as the binding index (see build_vertex_input).
         * Unused slots alias the primary buffer so the range stays valid. */
        VkBuffer binds[16]; VkDeviceSize offs[16] = {0};
        for (uint32_t b = 0; b < maxb; ++b) binds[b] = (b == 0) ? vb : (sb[b] ? sb[b] : vb);
        p_vkCmdBindVertexBuffers(c->cmd, 0, maxb, binds, offs);
    }
    if (idx) {
        VkBufferCreateInfo ibci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ibci.size = (VkDeviceSize)ni * 2; ibci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VkBuffer ib; VkDeviceMemory ibm;
        if (!p_vkCreateBuffer(g_device, &ibci, NULL, &ib) && !bind_host_mem(ib, &ibm)) {
            void *imap; p_vkMapMemory(g_device, ibm, 0, ibci.size, 0, &imap); memcpy(imap, idx, ibci.size); p_vkUnmapMemory(g_device, ibm);
            p_vkCmdBindIndexBuffer(c->cmd, ib, 0, VK_INDEX_TYPE_UINT16);
        }
    }
    p_vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 4, p->set, 0, NULL);
    struct { float size[2]; int tex_enabled, alpha_only, alpha_ref, color_only; } pc = {
        { (float)c->w, (float)c->h }, 1, 0, -1, 0 };
    p_vkCmdPushConstants(c->cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    p_vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    p_vkCmdSetViewport(c->cmd, 0, 1, &(VkViewport){ 0, 0, (float)c->vw, (float)c->vh, 0, 1 });
    p_vkCmdSetScissor(c->cmd, 0, 1, &(VkRect2D){ {c->vx, c->vy}, {(uint32_t)c->vw, (uint32_t)c->vh} });
    if (idx) p_vkCmdDrawIndexed(c->cmd, ni, 1, 0, 0, 0);
    else p_vkCmdDraw(c->cmd, nv, 1, 0, 0);
    c->tris += nv / 3;
    g_real_draws++;
    return 0;
}

static void copy_to_image(vk_tex *t, VkBuffer src, uint32_t layers, uint32_t pitch);
static void draw_rhw(mr_context *c, const mr_draw_state *st, int prim,
                     const void *v, uint32_t nv, const uint16_t *idx, uint32_t ni) {
    if (!c) return;
    begin_pass(c);
    uint8_t *vin = malloc(nv * 48);
    if (!vin) { set_errorf("out of memory"); return; }
    fill_vin(vin, (const mr_vertex_rhw*)v, nv);
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = nv * 48; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vb; VkDeviceMemory vbm;
    if (p_vkCreateBuffer(g_device, &bci, NULL, &vb) || bind_host_mem(vb, &vbm)) { free(vin); return; }
    void *map; p_vkMapMemory(g_device, vbm, 0, nv * 48, 0, &map);
    memcpy(map, vin, nv * 48); p_vkUnmapMemory(g_device, vbm);
    p_vkCmdBindVertexBuffers(c->cmd, 0, 1, &vb, (VkDeviceSize[]){ 0 });
    if (idx) {
        VkBufferCreateInfo ibci = bci; ibci.size = ni * 2; ibci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VkBuffer ib; VkDeviceMemory ibm;
        if (!p_vkCreateBuffer(g_device, &ibci, NULL, &ib) && !bind_host_mem(ib, &ibm)) {
            void *imap; p_vkMapMemory(g_device, ibm, 0, ni * 2, 0, &imap);
            memcpy(imap, idx, ni * 2); p_vkUnmapMemory(g_device, ibm);
            p_vkCmdBindIndexBuffer(c->cmd, ib, 0, VK_INDEX_TYPE_UINT16);
        }
    }
    /* Bind the texture's descriptor set (the fragment shader samples
     * binding 0 unconditionally) and push the draw state the shader reads.
     * Untextured draws sample the white texture; the shader's tex_enabled
     * flag then makes the result the diffuse colour. */
    uint32_t tex = st->texture ? st->texture : g_white;
    if (tex_bind(g_textures[tex])) {
        /* fall back to white so the draw still binds a valid set */
        if (tex != g_white) tex_bind(g_textures[g_white]);
        tex = g_white;
    }
    VkDescriptorSet dset = g_textures[tex] ? g_textures[tex]->ds : VK_NULL_HANDLE;
    if (dset) p_vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_pipeline_layout, 0, 1, &dset, 0, NULL);
    struct { float size[2]; int tex_enabled, alpha_only, alpha_ref, color_only; } pc = {
        { (float)c->w, (float)c->h },
        st->texture ? 1 : 0,
        st->texture_alpha_only ? 1 : 0,
        st->alpha_test_ref < 0 ? -1 : (int)st->alpha_test_ref,
        st->texture_color_only ? 1 : 0 };
    p_vkCmdPushConstants(c->cmd, g_pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkPipeline p = (st->depth_enable ? g_rhw_pipeline_d : g_rhw_pipeline);
    p_vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    p_vkCmdSetViewport(c->cmd, 0, 1, &(VkViewport){ 0, 0, (float)c->vw, (float)c->vh, 0, 1 });
    p_vkCmdSetScissor(c->cmd, 0, 1, &(VkRect2D){ {c->vx, c->vy}, {c->vw, c->vh} });
    if (idx) p_vkCmdDrawIndexed(c->cmd, ni, 1, 0, 0, 0);
    else p_vkCmdDraw(c->cmd, nv, 1, 0, 0);
    c->tris += nv / 3;
}

int mr_draw_rhw(mr_context *c, const mr_draw_state *st, int prim, const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) {
    (void)stride; draw_rhw(c, st, prim, v, nv, idx, ni); return MR_OK;
}
int mr_draw_program(mr_context *c, const mr_program_state *st, int prim, const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) {
    if (draw_program(c, st, prim, v, stride, nv, idx, ni) == 0) return MR_OK;
    if (g_real_fallbacks < 200) fprintf(stderr, "[shader] fallback %llu: %s\n", (unsigned long long)(g_real_fallbacks + 1), mr_last_error());
    g_real_fallbacks++;
    (void)stride; mr_draw_state ds = {0}; ds.texture = st->samplers[0].texture; ds.depth_enable = st->depth_enable;
    draw_rhw(c, &ds, prim, v, nv, idx, ni); return MR_OK;
}
int mr_draw_fixed_rhw(mr_context *c, const mr_program_state *st, int prim, const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) { return mr_draw_program(c, st, prim, v, stride, nv, idx, ni); }
int mr_draw_program_rhw(mr_context *c, const mr_program_state *st, int prim, const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) { return mr_draw_program(c, st, prim, v, stride, nv, idx, ni); }
int mr_draw_fixed_clip(mr_context *c, const mr_program_state *st, int prim, const void *v, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) { return mr_draw_program(c, st, prim, v, stride, nv, idx, ni); }

/* ----------------------------------------------------------- textures */
uint32_t mr_texture_create(mr_context *c, int w, int h, const void *bgra, size_t pitch) {
    (void)c; if (w <= 0 || h <= 0) return 0;
    uint32_t id = tex_alloc(); if (!id) return 0;
    vk_tex *t = calloc(1, sizeof *t);
    t->width = w; t->height = h; t->depth = 1; t->format = VK_FORMAT_B8G8R8A8_UNORM;
    if (make_image(w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &t->image, &t->memory)) { free(t); return 0; }
    if (bgra) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = h * pitch; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer sb; VkDeviceMemory sbm;
        if (!p_vkCreateBuffer(g_device, &bci, NULL, &sb) && !bind_host_mem(sb, &sbm)) {
            void *map; p_vkMapMemory(g_device, sbm, 0, h * pitch, 0, &map);
            for (int y = 0; y < h; y++) memcpy((uint8_t*)map + y * w * 4, (const uint8_t*)bgra + y * pitch, w * 4);
            p_vkUnmapMemory(g_device, sbm);
            copy_to_image(t, sb, h, pitch);
        }
    }
    g_textures[id] = t; g_cached_count++; g_cached_bytes += w * h * 4;
    return id;
}
int mr_texture_update(mr_context *c, uint32_t id, const void *bgra, size_t pitch) {
    (void)c; if (!id || !bgra) return -1; vk_tex *t = g_textures[id]; if (!t) return -1;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = t->height * pitch; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer sb; VkDeviceMemory sbm;
    if (p_vkCreateBuffer(g_device, &bci, NULL, &sb) || bind_host_mem(sb, &sbm)) return -1;
    void *map; p_vkMapMemory(g_device, sbm, 0, bci.size, 0, &map);
    for (int y = 0; y < t->height; y++) memcpy((uint8_t*)map + y * t->width * 4, (const uint8_t*)bgra + y * pitch, t->width * 4);
    p_vkUnmapMemory(g_device, sbm);
    copy_to_image(t, sb, t->height, pitch);
    p_vkDestroyBuffer(g_device, sb, NULL); p_vkFreeMemory(g_device, sbm, NULL);
    return 0;
}
void mr_texture_destroy(mr_context *c, uint32_t id) {
    (void)c; if (!id || id >= MR_MAX_TEX) return; vk_tex *t = g_textures[id]; if (!t) return;
    p_vkDestroyImage(g_device, t->image, NULL); p_vkFreeMemory(g_device, t->memory, NULL);
    g_cached_count--; g_cached_bytes -= t->width * t->height * 4; free(t); g_textures[id] = NULL;
}

static void copy_to_image(vk_tex *t, VkBuffer src, uint32_t layers, uint32_t pitch) {
    (void)layers;
    /* Record the upload on a dedicated command buffer: the queue is not a
     * command buffer and vkCmd* on it is undefined behavior (validation
     * VUID-vkCmdPipelineBarrier-commandBuffer-parameter). */
    VkCommandBufferAllocateInfo caib = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    caib.commandPool = g_cmd_pool; caib.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; caib.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (p_vkAllocateCommandBuffers(g_device, &caib, &cb) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(cb, &cbi) != VK_SUCCESS) { p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &cb); return; }
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = t->image;
    barrier.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy bic = {0};
    bic.imageExtent = (VkExtent3D){ t->width, t->height, 1 };
    bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bic.bufferRowLength = pitch / 4;
    p_vkCmdCopyBufferToImage(cb, src, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    p_vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &cb);
}
uint32_t mr_texture_find_cached(mr_context *c, uint64_t key) { (void)c; for (uint32_t i = 1; i < MR_MAX_TEX; i++) if (g_textures[i] && g_texture_keys[i] == key) return i; return 0; }
uint32_t mr_texture_create_cached(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch) { uint32_t id = mr_texture_create(c, w, h, bgra, pitch); if (id) g_texture_keys[id] = key; return id; }
uint32_t mr_texture_create_cached_nomip(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch) { return mr_texture_create_cached(c, key, w, h, bgra, pitch); }
/* The renderer's pipeline samples only the stage-0 texture through a 2D
 * view, so a cube map is flattened into a 6x1 vertical strip of its faces
 * (D3D order: +X,-X,+Y,-Y,+Z,-Z). The draw is accepted, the texture is
 * cached under its content key, and no cube view is ever bound, so the
 * sampler/view-type validation rules cannot be violated. */
uint32_t mr_texture_create_cube_cached(mr_context *c, uint64_t key, int edge, const void *faces[6], size_t pitch) {
    uint32_t hit = mr_texture_find_cached(c, key); if (hit) return hit;
    if (!key || edge <= 0 || !faces || pitch < (size_t)edge * 4) return 0;
    VkPhysicalDeviceProperties props; p_vkGetPhysicalDeviceProperties(g_phys, &props);
    if (edge > (int)props.limits.maxImageDimension2D / 6) return 0;
    size_t face_bytes = (size_t)edge * edge * 4;
    uint8_t *strip = malloc(face_bytes * 6);
    if (!strip) return 0;
    for (int f = 0; f < 6; f++) {
        if (!faces[f]) { free(strip); return 0; }
        for (int y = 0; y < edge; y++) memcpy(strip + ((size_t)f * edge + y) * edge * 4,
                                              (const uint8_t *)faces[f] + y * pitch, edge * 4);
    }
    uint32_t id = mr_texture_create(c, edge, edge * 6, strip, edge * 4);
    free(strip);
    if (id) g_texture_keys[id] = key;
    return id;
}
uint32_t mr_texture_create_volume_cached(mr_context *c, uint64_t key, int w, int h, int d, const void *bgra, size_t pitch, size_t slice) { (void)c; (void)key; (void)w; (void)h; (void)d; (void)bgra; (void)pitch; (void)slice; return 0; }
void mr_texture_bindings_begin(void) { }
void mr_texture_bindings_end(void) { }
void mr_texture_cache_counters(uint64_t *hits, uint64_t *up, uint64_t *ev) { if (hits) *hits = 0; if (up) *up = g_cached_count; if (ev) *ev = 0; }

/* --------------------------------------------------------- buffers */
typedef struct { VkBuffer buf; VkDeviceMemory mem; size_t len; } vk_buf;
void *mr_buffer_create(const void *bytes, size_t len) {
    vk_buf *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = len; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateBuffer(g_device, &bci, NULL, &b->buf) || bind_host_mem(b->buf, &b->mem)) { free(b); return NULL; }
    b->len = len;
    if (bytes) { void *map; p_vkMapMemory(g_device, b->mem, 0, len, 0, &map); memcpy(map, bytes, len); p_vkUnmapMemory(g_device, b->mem); }
    return b;
}
void mr_buffer_release(void *b) { if (b) { vk_buf *x = b; p_vkDestroyBuffer(g_device, x->buf, NULL); p_vkFreeMemory(g_device, x->mem, NULL); free(x); } }
const void *mr_buffer_contents(const void *b) { (void)b; return NULL; }
size_t mr_buffer_length(const void *b) { vk_buf *x = (vk_buf*)b; return x ? x->len : 0; }
int mr_fast_paths_enabled(void) { return 0; }
void mr_set_fast_paths(int e) { (void)e; }

/* --------------------------------------------------------- commit */
int mr_commit_async(void (*done)(void *arg, int ok), void *arg) {
    p_vkQueueWaitIdle(g_queue);
    if (done) done(arg, 1);
    return MR_OK;
}
const char *mr_last_commit_error(void) { return ""; }
void *mr_shared_device(void) { return NULL; }

/* --------------------------------------------------------- stats */
uint64_t mr_triangles_drawn(const mr_context *c) { return c ? c->tris : 0; }
uint32_t mr_cached_texture_count(const mr_context *c) { (void)c; return g_cached_count; }
uint64_t mr_cached_texture_bytes(const mr_context *c) { (void)c; return g_cached_bytes; }
void mr_pipeline_stats(mr_pipeline_stats_t *out) { if (out) memset(out, 0, sizeof *out); }
void mr_program_compile_stats(uint64_t *count, uint64_t *ns) { if (count) *count = g_real_draws; if (ns) *ns = g_real_fallbacks; }
void mr_shader_created(int pixel, const uint32_t *tokens, size_t bytes, uint64_t key) { (void)pixel; (void)tokens; (void)bytes; (void)key; }
void mr_pipeline_cache_flush(void) { }
void mr_set_overlay_target(mr_context *c) { (void)c; }
int mr_width(const mr_context *c) { return c ? c->w : 0; }
int mr_height(const mr_context *c) { return c ? c->h : 0; }

void mr_draw_traffic_stats(uint64_t *resident, uint64_t *arena, uint64_t *folded) {
    if (resident) *resident = 0; if (arena) *arena = 0; if (folded) *folded = 0;
}
int mr_blit_target_to_scaled(mr_context *c, void *dst, uint32_t dst_w, uint32_t dst_h) {
    if (!c) return -1;
    /* The blit's cb is submitted (and waited) before the source's frame
     * command buffer is ever queued, so the pending frame must reach the
     * GPU first or the blit would read the previous frame's content.
     * submit_frame closes the pass, submits and re-begins c->cmd; the
     * colour ends in COLOR_ATTACHMENT exactly as the src barrier below
     * assumes. */
    submit_frame(c);   /* closes the pass itself (line 638) */
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_cmd_pool; cai.commandBufferCount = 1;
    VkCommandBuffer cb; p_vkAllocateCommandBuffers(g_device, &cai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    p_vkBeginCommandBuffer(cb, &bi);
    /* Source: the eye renderer's colour is in COLOR_ATTACHMENT (after the
     * render pass or the CPU write). The blit reads it in TRANSFER_SRC;
     * restore it afterwards so the next pass can bind it.
     * Dst (the XR swapchain image, runtime-owned): UNDEFINED -> TRANSFER_DST
     * for the blit, then TRANSFER_DST -> COLOR_ATTACHMENT for the compositor.
     * The swapchain must carry VK_IMAGE_USAGE_TRANSFER_DST (requested via the
     * META additional-usage struct at create). */
    VkImageMemoryBarrier sb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    sb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sb.image = c->color; sb.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    db.srcAccessMask = 0; db.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; db.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    db.image = (VkImage)(uintptr_t)dst; db.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier pre[2] = { sb, db };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,NULL,0,NULL,2,pre);
    VkImageBlit blit = {0};
    blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[0] = (VkOffset3D){0,0,0}; blit.srcOffsets[1] = (VkOffset3D){(int32_t)c->w,(int32_t)c->h,1};
    blit.dstOffsets[0] = (VkOffset3D){0,0,0}; blit.dstOffsets[1] = (VkOffset3D){(int32_t)dst_w,(int32_t)dst_h,1};
    p_vkCmdBlitImage(cb, c->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkImage)(uintptr_t)dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    sb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; sb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; db.dstAccessMask = 0;
    db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; db.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImageMemoryBarrier post[2] = { sb, db };
    /* The SRC restore's dstAccessMask (COLOR_ATTACHMENT_WRITE) requires the
     * color-attachment output stage: BOTTOM_OF_PIPE would drop the ordering
     * (VUID-vkCmdPipelineBarrier-dstAccessMask-02816) and let the next pass
     * overwrite the frame mid-blit. */
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,0,NULL,0,NULL,2,post);
    p_vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &cb);
    return MR_OK;
}

int mr_blit_target_to(mr_context *c, void *dst) {
    if (!c) return MR_ERR_ARGS;
    return mr_blit_target_to_scaled(c, dst, (uint32_t)c->w, (uint32_t)c->h);
}

int mr_write_framebuffer(mr_context *c, const void *in, size_t size) {
    if (!c || !in || size < (size_t)c->w * c->h * 4) return -1;
    close_pass(c);
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer sb; VkDeviceMemory sbm;
    if (p_vkCreateBuffer(g_device, &bci, NULL, &sb) || bind_host_mem(sb, &sbm)) return -1;
    void *map; p_vkMapMemory(g_device, sbm, 0, size, 0, &map);
    memcpy(map, in, size); p_vkUnmapMemory(g_device, sbm);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_cmd_pool; cai.commandBufferCount = 1;
    VkCommandBuffer cb; p_vkAllocateCommandBuffers(g_device, &cai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    p_vkBeginCommandBuffer(cb, &bi);
    /* The color target may be in UNDEFINED (fresh) or COLOR_ATTACHMENT (after
     * a render pass); transition it to TRANSFER_DST before the upload, then
     * back so the next render pass can bind it. */
    VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.srcAccessMask = 0; to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.image = c->color; to_dst.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,0,NULL,0,NULL,1,&to_dst);
    VkImageSubresourceLayers sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    VkBufferImageCopy bic = {0};
    bic.imageSubresource = sub; bic.imageExtent = (VkExtent3D){(uint32_t)c->w,(uint32_t)c->h,1};
    p_vkCmdCopyBufferToImage(cb, sb, c->color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier to_att = to_dst;
    to_att.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; to_att.dstAccessMask = 0;
    to_att.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_att.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,0,NULL,0,NULL,1,&to_att);
    p_vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    p_vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(g_queue);
    p_vkFreeCommandBuffers(g_device, g_cmd_pool, 1, &cb);
    p_vkDestroyBuffer(g_device, sb, NULL); p_vkFreeMemory(g_device, sbm, NULL);
    return MR_OK;
}
uint32_t mr_target_texture(mr_context *c) { return c ? c->target_id : 0; }
int mr_fxaa_target_to(mr_context *c, void *dst, float subpix) {
    (void)subpix; return mr_blit_target_to(c, dst);
}

/* Test-only: the raw handle (VkImage) of the context's colour target. The
 * XR shell takes swapchain handles from OpenXR directly, so this is NOT
 * part of the mr_* contract and is deliberately absent from
 * metalrenderer.h; it exists only for the lavapipe pixel test, which blits
 * between two renderer contexts. Declared locally by the test. */
void *mr_raw_color_image(mr_context *c) { return c ? (void *)(uintptr_t)c->color : NULL; }
