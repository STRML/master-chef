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
VK_PFN(vkResetCommandBuffer)
VK_PFN(vkFreeCommandBuffers)

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
    L(vkFreeCommandBuffers)
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

/* ------------------------------------------------------ shared pipeline */
static void make_pipeline(void) {
    VkShaderModuleCreateInfo vsi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsi.codeSize = vk_spv_rhw_vert_len; vsi.pCode = vk_spv_rhw_vert;
    VkShaderModule vs, ps;
    p_vkCreateShaderModule(g_device, &vsi, NULL, &vs);
    VkShaderModuleCreateInfo psi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    psi.codeSize = vk_spv_rhw_frag_len; psi.pCode = vk_spv_rhw_frag;
    p_vkCreateShaderModule(g_device, &psi, NULL, &ps);

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
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", NULL },
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

    /* Descriptor pool: 1024 sets. */
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1024; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    p_vkCreateDescriptorPool(g_device, &dpci, NULL, &g_desc_pool);

    p_vkDestroyShaderModule(g_device, vs, NULL);
    p_vkDestroyShaderModule(g_device, ps, NULL);
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
uint32_t mr_texture_create_cube_cached(mr_context *c, uint64_t key, int edge, const void *faces[6], size_t pitch) { (void)c; (void)key; (void)edge; (void)faces; (void)pitch; return 0; }
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
void mr_program_compile_stats(uint64_t *count, uint64_t *ns) { if (count) *count = 0; if (ns) *ns = 0; }
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
