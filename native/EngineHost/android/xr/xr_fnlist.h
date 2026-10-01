/* xr_fnlist.h - X-macro list of the OpenXR extension entry points the
 * shell resolves through xrGetInstanceProcAddr (XR_KHR_vulkan_enable2
 * functions; everything else the shell calls is OpenXR core and links
 * against the loader).
 *
 * Consumer defines XR_FN(name, params) before including. */

XR_FN(xrCreateVulkanInstanceKHR,
    (XrInstance, const XrVulkanInstanceCreateInfoKHR*, VkInstance*, VkResult*))
XR_FN(xrGetVulkanGraphicsDevice2KHR,
    (XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, VkPhysicalDevice*))
XR_FN(xrCreateVulkanDeviceKHR,
    (XrInstance, const XrVulkanDeviceCreateInfoKHR*, VkDevice*, VkResult*))
XR_FN(xrGetVulkanGraphicsRequirements2KHR,
    (XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR*))
XR_FN(xrGetVulkanInstanceExtensionsKHR,
    (XrInstance, XrSystemId, uint32_t, uint32_t*, char*))
XR_FN(xrGetVulkanDeviceExtensionsKHR,
    (XrInstance, XrSystemId, uint32_t, uint32_t*, char*))
