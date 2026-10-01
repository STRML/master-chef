/*
 * xr_shell.c - OpenXR shell: session, per-eye Vulkan swapchains, head
 * pose, frame loop.
 *
 * Section 1 (pure math): quaternion/pose/matrix helpers over OpenXR
 * core types. Compiled and unit-tested on the macOS host; no runtime
 * calls.
 *
 * Section 2 (runtime, XR_USE_GRAPHICS_API_VULKAN only): the Android
 * OpenXR flow: instance (XR_KHR_android_create_instance) -> system ->
 * Vulkan via XR_KHR_vulkan_enable2 -> session -> per-eye swapchains
 * (XR_META_vulkan_swapchain_create_info) -> view reference space ->
 * frame loop that blits the engine's mr_context into each eye image
 * and submits a projection layer plus the quad UI stack.
 */
#include "xr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

/* ================================================================== */
/* Section 1: pose math (pure)                                       */
/* ================================================================== */

XrQuaternionf xr_quat_mul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf q;
    q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return q;
}

XrQuaternionf xr_quat_conjugate(XrQuaternionf q) {
    XrQuaternionf c = {-q.x, -q.y, -q.z, q.w};
    return c;
}

XrQuaternionf xr_quat_normalize(XrQuaternionf q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-12f) return xr_quat_identity();
    XrQuaternionf r = {q.x / n, q.y / n, q.z / n, q.w / n};
    return r;
}

XrVector3f xr_quat_rotate(XrQuaternionf q, XrVector3f v) {
    /* t = 2 * cross(q.xyz, v); v' = v + q.w*t + cross(q.xyz, t) */
    XrVector3f u = {q.x, q.y, q.z};
    XrVector3f t = {
        2.0f * (u.y * v.z - u.z * v.y),
        2.0f * (u.z * v.x - u.x * v.z),
        2.0f * (u.x * v.y - u.y * v.x),
    };
    XrVector3f r = {
        v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
        v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
        v.z + q.w * t.z + (u.x * t.y - u.y * t.x),
    };
    return r;
}

XrQuaternionf xr_quat_from_axis_angle(XrVector3f axis, float angle) {
    float len = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (len < 1e-12f) return xr_quat_identity();
    float s = sinf(angle * 0.5f) / len;
    XrQuaternionf q = {axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
    return q;
}

XrPosef xr_pose_compose(XrPosef a, XrPosef b) {
    XrPosef p;
    p.orientation = xr_quat_normalize(xr_quat_mul(a.orientation, b.orientation));
    XrVector3f rb = xr_quat_rotate(a.orientation, b.position);
    p.position.x = a.position.x + rb.x;
    p.position.y = a.position.y + rb.y;
    p.position.z = a.position.z + rb.z;
    return p;
}

XrPosef xr_pose_inverse(XrPosef p) {
    XrPosef q;
    q.orientation = xr_quat_conjugate(p.orientation);
    XrVector3f neg = {-p.position.x, -p.position.y, -p.position.z};
    q.position = xr_quat_rotate(q.orientation, neg);
    return q;
}

void xr_eye_poses(XrPosef head, float ipd, XrPosef out[2]) {
    /* Eyes sit on the head's local X axis: left at -ipd/2, right at
     * +ipd/2 (OpenXR: +X is to the right). */
    out[0] = head;
    out[1] = head;
    XrVector3f lx = {-ipd * 0.5f, 0.0f, 0.0f};
    XrVector3f rx = { ipd * 0.5f, 0.0f, 0.0f};
    XrVector3f lo = xr_quat_rotate(head.orientation, lx);
    XrVector3f ro = xr_quat_rotate(head.orientation, rx);
    out[0].position.x = head.position.x + lo.x;
    out[0].position.y = head.position.y + lo.y;
    out[0].position.z = head.position.z + lo.z;
    out[1].position.x = head.position.x + ro.x;
    out[1].position.y = head.position.y + ro.y;
    out[1].position.z = head.position.z + ro.z;
}

void xr_view_matrix(XrPosef p, float m[16]) {
    /* V = inv(rigid pose): R^T and -R^T t. Column-major. */
    XrQuaternionf qc = xr_quat_conjugate(p.orientation);
    float r00 = 1.0f - 2.0f * (qc.y * qc.y + qc.z * qc.z);
    float r01 = 2.0f * (qc.x * qc.y - qc.z * qc.w);
    float r02 = 2.0f * (qc.x * qc.z + qc.y * qc.w);
    float r10 = 2.0f * (qc.x * qc.y + qc.z * qc.w);
    float r11 = 1.0f - 2.0f * (qc.x * qc.x + qc.z * qc.z);
    float r12 = 2.0f * (qc.y * qc.z - qc.x * qc.w);
    float r20 = 2.0f * (qc.x * qc.z - qc.y * qc.w);
    float r21 = 2.0f * (qc.y * qc.z + qc.x * qc.w);
    float r22 = 1.0f - 2.0f * (qc.x * qc.x + qc.y * qc.y);
    /* rotation part of the conjugate quaternion = R^T of the pose;
     * apply it to -t as well. */
    XrVector3f neg = {-p.position.x, -p.position.y, -p.position.z};
    XrVector3f t = xr_quat_rotate(qc, neg);
    m[0] = r00; m[1] = r10; m[2] = r20; m[3] = 0.0f;
    m[4] = r01; m[5] = r11; m[6] = r21; m[7] = 0.0f;
    m[8] = r02; m[9] = r12; m[10] = r22; m[11] = 0.0f;
    m[12] = t.x; m[13] = t.y; m[14] = t.z; m[15] = 1.0f;
}

void xr_projection_matrix(XrFovf fov, float nearZ, float farZ, float m[16]) {
    float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
    float tanT = tanf(fov.angleUp), tanB = tanf(fov.angleDown);
    float a = 2.0f / (tanR - tanL);
    float b = (tanR + tanL) / (tanR - tanL);
    float c = 2.0f / (tanB - tanT);          /* negative: y flip */
    float d = (tanB + tanT) / (tanB - tanT);
    float e = farZ / (nearZ - farZ);          /* z: [near,far] -> [0,1] */
    float f = farZ * nearZ / (nearZ - farZ);
    memset(m, 0, 16 * sizeof(float));
    m[0] = a;  m[8] = b;  m[12] = 0.0f;
    m[5] = c;  m[9] = d;
    m[10] = e; m[14] = f;
    m[11] = -1.0f;
}

/* ================================================================== */
/* Section 2: runtime (Android + Vulkan). Compiled only when        */
/* XR_USE_GRAPHICS_API_VULKAN is set (the NDK build). The host     */
/* unit tests compile section 1 only.                               */
/* ================================================================== */
#ifdef XR_USE_GRAPHICS_API_VULKAN

#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>
#include "../../metalrenderer.h"

#define XR_MAX_IMAGES 8
#define XR_NEAR 0.05f
#define XR_FAR 2000.0f
#define XR_IPD 0.0635f

static XrInstance g_instance;
static char g_last_error[256];
static void xr_fail(const char *what, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance) xrResultToString(g_instance, r, buf);
    else snprintf(buf, sizeof buf, "XrResult %d", (int)r);
    snprintf(g_last_error, sizeof g_last_error, "%s: %s", what, buf);
    fprintf(stderr, "[xr] %s\n", g_last_error);
}
const char *xr_shell_last_error(void) { return g_last_error; }

#define XR_FN(name, params) typedef XrResult (XRAPI_PTR *PFN_##name) params; \
    static PFN_##name name;
#include "xr_fnlist.h"

typedef struct xr_eye {
    XrSwapchain         swapchain;
    XrSwapchainImageVulkan2KHR images[XR_MAX_IMAGES];
    uint32_t            imageCount;
    uint32_t            width, height;
    XrFovf              fov;
    XrPosef             pose;
    mr_context         *renderer;
} xr_eye;

struct xr_shell {
    XrInstance          instance;
    XrSystemId          system;
    XrSession         session;
    XrSpace           space;          /* head-anchored view space */
    VkInstance        vkInstance;
    VkPhysicalDevice  vkPhysical;
    VkDevice          vkDevice;
    uint32_t        vkQueueFamily, vkQueueIndex;
    xr_eye          eye[2];
    XrView            views[2];
    XrViewConfigurationView cfgViews[2];
    XrCompositionLayerProjectionView projViews[2];
    XrCompositionLayerProjection proj;
    xr_quads          quads;
    int               frames;
    int               maxFrames;
    atomic_int        exitRequested;
    bool              sessionBegun;
    bool              showQuads;
    XrEnvironmentBlendMode blend;
    xr_lc lc;
    mr_context *engine_target;
    int engineW, engineH;         /* newest engine frame dimensions   */
};

static void xr_bind_ext_fns(XrInstance inst) {
#undef XR_FN
#define XR_FN(name, params) xrGetInstanceProcAddr(inst, #name, (PFN_xrVoidFunction*)&name);
#include "xr_fnlist.h"
}


/* ---- instance + system ---- */
static int xr_make_instance(xr_shell *s, const xr_shell_config *cfg) {
    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName,
             XR_MAX_APPLICATION_NAME_SIZE, "%s", cfg->app_name ? cfg->app_name : "halo-xr");
    snprintf(ici.applicationInfo.engineName,
             XR_MAX_ENGINE_NAME_SIZE, "%s", "halo-ce-compat");
    ici.applicationInfo.engineVersion = 1;
    /* Request the 1.0 API: a runtime that only implements OpenXR 1.0
     * (XR_CURRENT_API_VERSION here is 1.1.x) rejects the instance with
     * XR_ERROR_API_VERSION_UNSUPPORTED. Everything the shell uses is
     * 1.0 core + the enable2/ANDROID/META extensions. */
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = cfg->application_vm;
    android.applicationActivity = cfg->application_activity;
    ici.next = &android;
    /* Extension list: Vulkan enable2 + META swapchain create info +
     * (optional) color scale bias for tinted quads. */
    static const char *req[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME,
        XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME,
    };
    ici.enabledExtensionNames = req;
    ici.enabledExtensionCount = sizeof req / sizeof req[0];
    XrResult r = xrCreateInstance(&ici, &s->instance);
    if (XR_FAILED(r)) { xr_fail("xrCreateInstance", r); return -1; }
    g_instance = s->instance;
    xr_bind_ext_fns(s->instance);
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO, NULL, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    r = xrGetSystem(s->instance, &sgi, &s->system);
    if (XR_FAILED(r)) { xr_fail("xrGetSystem", r); return -1; }
    return 0;
}
#include "xr_runtime.inl"

#endif /* XR_USE_GRAPHICS_API_VULKAN */
