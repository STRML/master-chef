/* xr_runtime.inl - Vulkan enable2 + session + per-eye swapchains.
 * Included by xr_shell.c inside the XR_USE_GRAPHICS_API_VULKAN block. */

static void xr_poll_events(xr_shell *s);
static int xr_locate_views(xr_shell *s, XrTime t);
static int xr_make_instance(xr_shell *s, const xr_shell_config *cfg);
static int xr_make_vulkan(xr_shell *s, const xr_shell_config *cfg) {
    (void)cfg;
    /* 1. The runtime's suggested Vulkan instance. */
    XrVulkanInstanceCreateInfoKHR ici = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    ici.systemId = s->system;
    ici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    VkApplicationInfo vai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    vai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vi = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vi.pApplicationInfo = &vai;
    XrResult r;
    VkResult vr = VK_SUCCESS;
    if (!xrCreateVulkanInstanceKHR) { xr_fail("xrCreateVulkanInstanceKHR", XR_ERROR_FUNCTION_UNSUPPORTED); return -1; }
    r = xrCreateVulkanInstanceKHR(s->instance, &ici, &s->vkInstance, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { xr_fail("xrCreateVulkanInstanceKHR", r); return -1; }

    /* 2. The XR-recommended physical device. */
    XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = s->system;
    gi.vulkanInstance = s->vkInstance;
    if (!xrGetVulkanGraphicsDevice2KHR) { xr_fail("xrGetVulkanGraphicsDevice2KHR", XR_ERROR_FUNCTION_UNSUPPORTED); return -1; }
    r = xrGetVulkanGraphicsDevice2KHR(s->instance, &gi, &s->vkPhysical);
    if (XR_FAILED(r)) { xr_fail("xrGetVulkanGraphicsDevice2KHR", r); return -1; }

    /* 3. Logical device exposing the graphics queue family. */
    uint32_t qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->vkPhysical, &qf, NULL);
    VkQueueFamilyProperties props[16];
    if (qf > 16) qf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(s->vkPhysical, &qf, props);
    for (uint32_t i = 0; i < qf; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s->vkQueueFamily = i; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = s->vkQueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    XrVulkanDeviceCreateInfoKHR dci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    dci.systemId = s->system;
    dci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    dci.vulkanPhysicalDevice = s->vkPhysical;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qci;
    if (!xrCreateVulkanDeviceKHR) { xr_fail("xrCreateVulkanDeviceKHR", XR_ERROR_FUNCTION_UNSUPPORTED); return -1; }
    r = xrCreateVulkanDeviceKHR(s->instance, &dci, &s->vkDevice, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) { xr_fail("xrCreateVulkanDeviceKHR", r); return -1; }

    /* 4. Session with the Vulkan2 graphics binding. */
    XrGraphicsBindingVulkan2KHR bind = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    bind.instance = s->vkInstance;
    bind.physicalDevice = s->vkPhysical;
    bind.device = s->vkDevice;
    bind.queueFamilyIndex = s->vkQueueFamily;
    bind.queueIndex = 0;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &bind;
    sci.systemId = s->system;
    XrResult r2 = xrCreateSession(s->instance, &sci, &s->session);
    if (XR_FAILED(r2)) { xr_fail("xrCreateSession", r2); return -1; }

    /* 5. Head-anchored view space. */
    XrReferenceSpaceCreateInfo rsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rsi.poseInReferenceSpace.orientation = xr_quat_identity();
    XrResult r3 = xrCreateReferenceSpace(s->session, &rsi, &s->space);
    if (XR_FAILED(r3)) { xr_fail("xrCreateReferenceSpace", r3); return -1; }

    /* 6. Bind the renderer to this runtime-recommended device. The
     * per-eye swapchains (xr_make_eye_swapchain) and the engine's own
     * backbuffer are then created on the same device, so the renderer's
     * blit of the engine frame into a swapchain image is a same-device
     * operation (a cross-device blit is invalid in Vulkan). */
    if (mr_adopt_vulkan(s->vkInstance, s->vkPhysical, s->vkDevice, s->vkQueueFamily) != 0) {
        xr_fail("mr_adopt_vulkan", XR_ERROR_RUNTIME_FAILURE); return -1;
    }
    return 0;
}

/* Per-eye XR swapchain sized to the recommended view config rect,
 * B8G8R8A8_UNORM, colour-attachment usage via the META extension. */
static int xr_make_eye_swapchain(xr_shell *s, int e) {
    xr_eye *eye = &s->eye[e];
    eye->width  = s->cfgViews[e].recommendedImageRectWidth;
    eye->height = s->cfgViews[e].recommendedImageRectHeight;
    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = VK_FORMAT_B8G8R8A8_UNORM;
    sci.sampleCount = 1;
    sci.width = eye->width;
    sci.height = eye->height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    XrVulkanSwapchainCreateInfoMETA meta = {XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
    /* TRANSFER_DST: the renderer's mr_blit_target_to copies the engine
     * frame into the swapchain image as a blit destination. The compositor
     * reads it in COLOR_ATTACHMENT; the blit restores it there. */
    meta.additionalUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.next = &meta;
    XrResult r = xrCreateSwapchain(s->session, &sci, &eye->swapchain);
    if (XR_FAILED(r)) { xr_fail("xrCreateSwapchain", r); return -1; }
    uint32_t n = 0;
    r = xrEnumerateSwapchainImages(eye->swapchain, XR_MAX_IMAGES, &n,
        (XrSwapchainImageBaseHeader*)eye->images);
    if (XR_FAILED(r) || n == 0) { xr_fail("xrEnumerateSwapchainImages", r); return -1; }
    eye->imageCount = n;
    eye->renderer = mr_create((int)eye->width, (int)eye->height);
    if (!eye->renderer) { xr_fail("mr_create", XR_ERROR_RUNTIME_FAILURE); return -1; }
    /* The engine frame is stale against the new target; the next
     * xr_shell_set_engine_frame resizes the renderer to the engine
     * resolution again. */
    s->engineW = s->engineH = 0;
    return 0;
}

static int xr_make_swapchains(xr_shell *s) {
    uint32_t vc = 0;
    XrResult r = xrEnumerateViewConfigurationViews(s->instance, s->system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, s->cfgViews);
    if (XR_FAILED(r) || vc < 2) { xr_fail("xrEnumerateViewConfigurationViews", r); return -1; }
    for (int e = 0; e < 2; e++)
        if (xr_make_eye_swapchain(s, e) != 0) return -1;
    return 0;
}

/* XR swapchain size change: re-enumerate the view configuration and
 * rebuild any eye whose recommended image rect changed (dynamic
 * resolution / refresh-rate switches on Quest; the OpenXR spec has no
 * dedicated event, so apps re-check the recommended rect). */
static int xr_check_view_config(xr_shell *s) {
    XrViewConfigurationView views[2];
    uint32_t vc = 0;
    XrResult r = xrEnumerateViewConfigurationViews(s->instance, s->system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, views);
    if (XR_FAILED(r) || vc < 2) { xr_fail("xrEnumerateViewConfigurationViews", r); return -1; }
    for (int e = 0; e < 2; e++) {
        if (views[e].recommendedImageRectWidth == s->eye[e].width &&
            views[e].recommendedImageRectHeight == s->eye[e].height)
            continue;
        xr_eye *eye = &s->eye[e];
        fprintf(stderr, "[xr] eye %d swapchain resize %ux%u -> %ux%u\n",
                e, eye->width, eye->height,
                views[e].recommendedImageRectWidth,
                views[e].recommendedImageRectHeight);
        if (eye->renderer) { mr_destroy(eye->renderer); eye->renderer = NULL; }
        if (eye->swapchain) { xrDestroySwapchain(eye->swapchain); eye->swapchain = XR_NULL_HANDLE; }
        s->cfgViews[e] = views[e];
        if (xr_make_eye_swapchain(s, e) != 0) return -1;
    }
    return 0;
}

/* UI quad swapchains: one small color-attachment per visible quad,
 * painted with the quad tint through the mr_* contract. */
static int xr_make_quad_swapchains(xr_shell *s) {
    for (uint32_t i = 0; i < s->quads.count; i++) {
        xr_quad *q = &s->quads.quad[i];
        if (!q->in_use || q->swapchain) continue;
        uint32_t w = q->width ? q->width : 512;
        uint32_t h = q->height ? q->height : 256;
        XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = VK_FORMAT_B8G8R8A8_UNORM;
        sci.sampleCount = 1;
        sci.width = w;
        sci.height = h;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        XrVulkanSwapchainCreateInfoMETA meta = {XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
        meta.additionalUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.next = &meta;
        XrResult r = xrCreateSwapchain(s->session, &sci, &q->swapchain);
        if (XR_FAILED(r)) { xr_fail("xrCreateSwapchain(quad)", r); return -1; }
        XrSwapchainImageVulkan2KHR imgs[XR_MAX_IMAGES];
        uint32_t n = 0;
        r = xrEnumerateSwapchainImages(q->swapchain, XR_MAX_IMAGES, &n,
            (XrSwapchainImageBaseHeader*)imgs);
        if (XR_FAILED(r) || n == 0) { xr_fail("xrEnumerateSwapchainImages(quad)", r); return -1; }
        mr_context *rc = mr_create((int)w, (int)h);
        if (!rc) { xr_fail("mr_create(quad)", XR_ERROR_RUNTIME_FAILURE); return -1; }
        /* Paint the tint: D3DCOLOR 0xAARRGGBB. */
        uint32_t col = ((uint32_t)(q->tint.a * 255.0f) << 24) |
                       ((uint32_t)(q->tint.r * 255.0f) << 16) |
                       ((uint32_t)(q->tint.g * 255.0f) << 8)  |
                       ((uint32_t)(q->tint.b * 255.0f));
        mr_clear(rc, col);
        for (uint32_t k = 0; k < n; k++)
            mr_blit_target_to(rc, (void*)imgs[k].image);
        mr_commit_async(NULL, NULL);
        xr_quads_bind_swapchain(&s->quads, q->id, q->swapchain, w, h);
        /* rc is owned by the quad for the lifetime of the session;
         * the shell destroys it with the quad in xr_shell_destroy. */
        q->renderer = rc;
    }
    return 0;
}

/* Locate the stereo views into the head space (the per-eye poses the
 * projection layer carries). */
static int xr_locate_views(xr_shell *s, XrTime t) {
    XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = t;
    vli.space = s->space;
    uint32_t n = 2;
    XrViewState vst = {XR_TYPE_VIEW_STATE};
    for (int i = 0; i < 2; i++) {
        s->views[i].type = XR_TYPE_VIEW;
        s->views[i].next = NULL;
    }
    XrResult r = xrLocateViews(s->session, &vli, &vst, 2, &n, s->views);
    if (XR_FAILED(r)) { xr_fail("xrLocateViews", r); return -1; }
    if (!(vst.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(vst.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        xr_fail("xrLocateViews invalid", XR_ERROR_RUNTIME_UNAVAILABLE);
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        s->eye[i].pose = s->views[i].pose;
        s->eye[i].fov = s->views[i].fov;
    }
    return 0;
}

/* One frame: acquire, let the engine draw into its target, blit the
 * target into each eye's swapchain image, release, submit the
 * projection + quad layers. */
int xr_shell_frame(xr_shell *s) {
    xr_poll_events(s);
    if (xr_lc_should_exit(&s->lc)) return 1;
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(s->session, &(XrFrameWaitInfo){XR_TYPE_FRAME_WAIT_INFO}, &fs);
    if (XR_FAILED(r)) { xr_fail("xrWaitFrame", r); return -1; }
    r = xrBeginFrame(s->session, &(XrFrameBeginInfo){XR_TYPE_FRAME_BEGIN_INFO});
    if (XR_FAILED(r)) { xr_fail("xrBeginFrame", r); return -1; }

    if (!fs.shouldRender) {
        xrEndFrame(s->session, &(XrFrameEndInfo){XR_TYPE_FRAME_END_INFO,
            NULL, fs.predictedDisplayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, NULL});
        return 1;
    }
    if (xr_check_view_config(s) != 0) return -1;
    if (xr_locate_views(s, fs.predictedDisplayTime) != 0) return -1;

    for (int e = 0; e < 2; e++) {
        xr_eye *eye = &s->eye[e];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        r = xrAcquireSwapchainImage(eye->swapchain, &acq, &idx);
        if (XR_FAILED(r)) { xr_fail("xrAcquireSwapchainImage", r); return -1; }
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, NULL, XR_INFINITE_DURATION};
        r = xrWaitSwapchainImage(eye->swapchain, &wi);
        if (XR_FAILED(r)) { xr_fail("xrWaitSwapchainImage", r); return -1; }
        /* The engine's D3D shim draws into its offscreen target; the
         * shell stretches it into the full swapchain image (GPU bilinear)
         * so the compositor sees the frame at any engine resolution.
         * The renderer's blit transitions the swapchain UNDEFINED->DST
         * and restores it to COLOR_ATTACHMENT for the compositor. */
        if (eye->renderer && (s->engine_target || s->engineW > 0)) {
            int b = mr_blit_target_to_scaled(eye->renderer, (void*)eye->images[idx].image,
                                             eye->width, eye->height);
            if (b != 0) { xr_fail("mr_blit_target_to", XR_ERROR_RUNTIME_FAILURE); return -1; }
        }
        XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        r = xrReleaseSwapchainImage(eye->swapchain, &rel);
        if (XR_FAILED(r)) { xr_fail("xrReleaseSwapchainImage", r); return -1; }

        s->projViews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        s->projViews[e].pose = eye->pose;
        s->projViews[e].fov = eye->fov;
        s->projViews[e].subImage.swapchain = eye->swapchain;
        s->projViews[e].subImage.imageRect.offset.x = 0;
        s->projViews[e].subImage.imageRect.offset.y = 0;
        s->projViews[e].subImage.imageRect.extent.width = (int32_t)eye->width;
        s->projViews[e].subImage.imageRect.extent.height = (int32_t)eye->height;
        s->projViews[e].subImage.imageArrayIndex = 0;
    }

    /* Layers: projection (content) first, then the quad UI stack
     * (frontmost). */
    const XrCompositionLayerBaseHeader *layers[1 + XR_QUADS_MAX];
    s->proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    s->proj.space = s->space;
    s->proj.viewCount = 2;
    s->proj.views = s->projViews;
    uint32_t nLayers = 0;
    layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&s->proj;
    if (s->showQuads) {
        uint32_t qn = xr_quads_build_layers(&s->quads);
        for (uint32_t i = 0; i < qn; i++)
            layers[nLayers++] = (const XrCompositionLayerBaseHeader*)&s->quads.layers[i];
    }
    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nLayers;
    fei.layers = layers;
    r = xrEndFrame(s->session, &fei);
    if (XR_FAILED(r)) { xr_fail("xrEndFrame", r); return -1; }
    s->frames++;
    return 0;
}

int xr_shell_run(xr_shell *s) {
    while (!atomic_load(&s->exitRequested)) {
        if (xr_lc_should_exit(&s->lc)) break;
        int f = xr_shell_frame(s);
        if (f < 0) return f;
        if (s->maxFrames > 0 && s->frames >= s->maxFrames) break;
    }
    return 0;
}

void xr_shell_request_exit(xr_shell *s) {
    if (s) atomic_store(&s->exitRequested, 1);
}

/* Poll XR events into the lifecycle FSM (session state, focus,
 * instance loss). Called from the frame loop before each frame. */
static void xr_poll_events(xr_shell *s) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s->instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            XrEventDataSessionStateChanged *sc = (XrEventDataSessionStateChanged*)&ev;
            xr_lc_send(&s->lc, xr_lc_event_from_session_state(sc->state));
            if (s->lc.actions & XR_LC_ACT_QUIT) xr_shell_request_exit(s);
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            xr_lc_send(&s->lc, XR_LC_INSTANCE_LOSS);
            xr_shell_request_exit(s);
            break;
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            break;
        default:
            break;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

XrPosef xr_shell_head_pose(const xr_shell *s) {
    XrPosef h = {xr_quat_identity(), {0,0,0}};
    /* Midpoint of the two eye poses (head space). */
    XrPosef l = s->eye[0].pose, r = s->eye[1].pose;
    h.orientation = xr_quat_normalize((XrQuaternionf){
        (l.orientation.x + r.orientation.x) * 0.5f,
        (l.orientation.y + r.orientation.y) * 0.5f,
        (l.orientation.z + r.orientation.z) * 0.5f,
        (l.orientation.w + r.orientation.w) * 0.5f});
    h.position.x = (l.position.x + r.position.x) * 0.5f;
    h.position.y = (l.position.y + r.position.y) * 0.5f;
    h.position.z = (l.position.z + r.position.z) * 0.5f;
    return h;
}

void xr_shell_destroy(xr_shell *s) {
    if (!s) return;
    if (s->session) {
        for (int e = 0; e < 2; e++) {
            if (s->eye[e].renderer) mr_destroy(s->eye[e].renderer);
            if (s->eye[e].swapchain) xrDestroySwapchain(s->eye[e].swapchain);
        }
        for (uint32_t i = 0; i < s->quads.count; i++) {
            if (s->quads.quad[i].renderer) mr_destroy(s->quads.quad[i].renderer);
            if (s->quads.quad[i].swapchain) xrDestroySwapchain(s->quads.quad[i].swapchain);
        }
        if (s->space) xrDestroySpace(s->space);
        if (s->sessionBegun) xrEndSession(s->session);
        xrDestroySession(s->session);
    }
    if (s->vkDevice) vkDestroyDevice(s->vkDevice, NULL);
    if (s->vkInstance) vkDestroyInstance(s->vkInstance, NULL);
    if (s->instance) xrDestroyInstance(s->instance);
    free(s);
}

xr_shell *xr_shell_create(const xr_shell_config *cfg) {
    xr_shell *s = (xr_shell*)calloc(1, sizeof *s);
    if (!s) { xr_fail("calloc", XR_ERROR_RUNTIME_FAILURE); return NULL; }
    s->maxFrames = cfg->max_frames;
    s->showQuads = cfg->show_quads;
    s->engine_target = cfg->engine_target;
    xr_lc_init(&s->lc);
    if (xr_make_instance(s, cfg) != 0) { xr_shell_destroy(s); return NULL; }
    if (xr_make_vulkan(s, cfg) != 0) { xr_shell_destroy(s); return NULL; }
    if (xr_make_swapchains(s) != 0) { xr_shell_destroy(s); return NULL; }
    xr_quads_init(&s->quads, s->space);
    if (cfg->show_quads) {
        xr_quads_default_layout(&s->quads);
        if (xr_make_quad_swapchains(s) != 0) { xr_shell_destroy(s); return NULL; }
    }
    /* Android sessions start READY; the runtime moves them to RUNNING
     * via events (poll pumps them). */
    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrResult r = xrBeginSession(s->session, &bi);
    if (XR_FAILED(r) && r != XR_ERROR_SESSION_RUNNING) {
        xr_fail("xrBeginSession", r);
    } else {
        s->sessionBegun = true;
    }
    return s;
}

/* ---- activity bridge (android_main.c) ---- */

XrInstance xr_shell_instance(const xr_shell *s) { return s ? s->instance : XR_NULL_HANDLE; }
XrSession  xr_shell_session(const xr_shell *s)  { return s ? s->session  : XR_NULL_HANDLE; }

void xr_shell_send_lc_event(xr_shell *s, xr_lc_event ev) {
    if (!s) return;
    xr_lc_send(&s->lc, ev);
    if (s->lc.actions & XR_LC_ACT_QUIT) xr_shell_request_exit(s);
}

int xr_shell_should_exit(const xr_shell *s) { return s && xr_lc_should_exit(&s->lc); }

/* Upload the newest engine frame (BGRA8) into each eye's render
 * target. The eye renderers are resized to the engine resolution
 * (the projection layer's imageRect keeps the XR swapchain extent, so
 * the compositor scales the content). Called from the XR render
 * thread, before xr_shell_frame. */
int xr_shell_set_engine_frame(xr_shell *s, const void *bgra, int width, int height) {
    if (!s || !bgra || width <= 0 || height <= 0) return -1;
    size_t need = (size_t)width * (size_t)height * 4;
    if (s->engineW != width || s->engineH != height) {
        for (int e = 0; e < 2; e++) {
            if (!s->eye[e].renderer) return -1;
            if (mr_resize(s->eye[e].renderer, width, height) != 0) {
                xr_fail("mr_resize", XR_ERROR_RUNTIME_FAILURE);
                return -1;
            }
        }
        s->engineW = width;
        s->engineH = height;
    }
    for (int e = 0; e < 2; e++) {
        if (mr_write_framebuffer(s->eye[e].renderer, bgra, need) != MR_OK) {
            xr_fail("mr_write_framebuffer", XR_ERROR_RUNTIME_FAILURE);
            return -1;
        }
    }
    return 0;
}
