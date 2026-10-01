/*
 * xr_quads.c - quad layer UI: a Z-ordered stack of billboard quads
 * anchored in the head space, with a per-quad swapchain the host binds
 * and a solid-color tint via XR_KHR_composition_layer_color_scale_bias.
 * Pure state management + struct building (no XR runtime calls); the
 * host's frame loop (xr_shell.c) submits the layers.
 */
#include "xr.h"
#include <string.h>

void xr_quads_init(xr_quads *qs, XrSpace space) {
    memset(qs, 0, sizeof *qs);
    qs->space = space;
}
uint32_t xr_quads_add(xr_quads *qs, XrPosef pose, XrExtent2Df size,
                      XrColor4f tint, int z) {
    if (qs->count >= XR_QUADS_MAX) return 0;
    uint32_t id = 1;
    for (uint32_t i = 0; i < qs->count; i++)
        if (qs->quad[i].id >= id) id = qs->quad[i].id + 1;
    /* Insert before the first quad with a higher z (ascending, stable:
     * equal z keeps insertion order). */
    uint32_t pos = qs->count;
    for (uint32_t i = 0; i < qs->count; i++) {
        if (qs->quad[i].z > z) { pos = i; break; }
    }
    memmove(&qs->quad[pos + 1], &qs->quad[pos],
            (qs->count - pos) * sizeof(qs->quad[pos]));
    qs->quad[pos].in_use = true;
    qs->quad[pos].id = id;
    qs->quad[pos].pose = pose;
    qs->quad[pos].size = size;
    qs->quad[pos].tint = tint;
    qs->quad[pos].z = z;
    qs->quad[pos].visible = true;
    qs->quad[pos].swapchain = XR_NULL_HANDLE;
    qs->quad[pos].width = 0;
    qs->quad[pos].height = 0;
    qs->count++;
    return id;
}

static xr_quad *xr_quads_find(xr_quads *qs, uint32_t id) {
    for (uint32_t i = 0; i < qs->count; i++)
        if (qs->quad[i].in_use && qs->quad[i].id == id) return &qs->quad[i];
    return NULL;
}

int xr_quads_bind_swapchain(xr_quads *qs, uint32_t id, XrSwapchain sc,
                            uint32_t width, uint32_t height) {
    xr_quad *q = xr_quads_find(qs, id);
    if (!q) return -1;
    q->swapchain = sc;
    q->width = width;
    q->height = height;
    return 0;
}

int xr_quads_set_visible(xr_quads *qs, uint32_t id, bool visible) {
    xr_quad *q = xr_quads_find(qs, id);
    if (!q) return -1;
    q->visible = visible;
    return 0;
}

int xr_quads_set_pose(xr_quads *qs, uint32_t id, XrPosef pose) {
    xr_quad *q = xr_quads_find(qs, id);
    if (!q) return -1;
    q->pose = pose;
    return 0;
}

int xr_quads_remove(xr_quads *qs, uint32_t id) {
    for (uint32_t i = 0; i < qs->count; i++) {
        if (qs->quad[i].in_use && qs->quad[i].id == id) {
            memmove(&qs->quad[i], &qs->quad[i + 1],
                  (qs->count - i - 1) * sizeof(qs->quad[i]));
            qs->count--;
            qs->quad[qs->count].in_use = false;
            return 0;
        }
    }
    return -1;
}

/* Build the visible XrCompositionLayerQuad array for xrEndFrame,
 * ascending z (painter's order; the last layer is frontmost). Quads
 * without a swapchain are skipped. Returns the layer count. */
uint32_t xr_quads_build_layers(xr_quads *qs) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < qs->count; i++) {
        xr_quad *q = &qs->quad[i];
        if (!q->in_use || !q->visible || !q->swapchain) continue;
        XrCompositionLayerQuad *L = &qs->layers[n];
        L->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        L->next = NULL;
        L->layerFlags = 0;
        L->space = qs->space;
        L->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        L->subImage.swapchain = q->swapchain;
        L->subImage.imageRect.offset.x = 0;
        L->subImage.imageRect.offset.y = 0;
        L->subImage.imageRect.extent.width = (int32_t)q->width;
        L->subImage.imageRect.extent.height = (int32_t)q->height;
        L->subImage.imageArrayIndex = 0;
        L->pose = q->pose;
        L->size = q->size;
        qs->layer_ptrs[n++] = (const XrCompositionLayerBaseHeader*)L;
    }
    return n;
}

/* Standard in-app HUD: a loading panel at 1.5 m, an exit button
 * bottom-right, both head-anchored (the space is the view space). */
void xr_quads_default_layout(xr_quads *qs) {
    XrPosef center = {xr_quat_identity(), {0.0f, 0.0f, -1.5f}};
    XrExtent2Df panel = {0.8f, 0.45f};
    XrColor4f white = {1.0f, 1.0f, 1.0f, 1.0f};
    XrColor4f red = {1.0f, 0.2f, 0.2f, 1.0f};
    xr_quads_add(qs, center, panel, white, 10);
    XrPosef btn = {xr_quat_identity(), {0.32f, -0.28f, -1.2f}};
    XrExtent2Df bs = {0.18f, 0.18f};
    xr_quads_add(qs, btn, bs, red, 20);
}
