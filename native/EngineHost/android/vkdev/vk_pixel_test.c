/* vk_pixel_test.c - standalone pixel correctness test for the Vulkan
 * renderer (vulkanrenderer.c). Compiled and run natively inside the
 * halo-vk-dev container (glibc + lavapipe). No engine, no game data:
 * drives the mr_* API directly and asserts the exact pixels the
 * readback path returns, proving the full render pass -> submit ->
 * readback loop produces correct output.
 *
 * Build (inside container; vulkanrenderer.c references the vkshader
 * translation layer, so link it plus the MojoShader SPIRV TUs, same set as
 * vkshader_test.c above and the -D flags in vkbuild.sh):
 *   gcc -O1 -g -std=gnu11 -w -I native/EngineHost \
 *       native/EngineHost/android/vkdev/vk_pixel_test.c \
 *       native/EngineHost/vulkanrenderer.c native/EngineHost/vkshader.c \
 *       third_party/mojoshader/mojoshader.c third_party/mojoshader/mojoshader_common.c \
 *       third_party/mojoshader/profiles/mojoshader_profile_common.c \
 *       third_party/mojoshader/profiles/mojoshader_profile_spirv.c \
 *       -o /tmp/vk_pixel_test -lvulkan -lm
 * Run: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json /tmp/vk_pixel_test
 *
 * Exit 0 + "VK_PIXEL PASS" = the renderer produces correct pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "metalrenderer.h"

static int failures = 0;

static void check(const char *what, int px, int py, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s (%d,%d): got %d want %d\n", what, px, py, got, want);
        failures++;
    }
}

/* The renderer's colour target is B8G8R8A8 (memory bytes B,G,R,A) and
 * mr_read_framebuffer copies that raw memory, so the host sees BGRA. */
static uint32_t getbgra(const uint8_t *p, int x, int y, int w) {
    const uint8_t *q = p + ((size_t)y * w + x) * 4;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/* Test-only accessor defined in vulkanrenderer.c (deliberately not part of
 * the metalrenderer.h contract): the raw VkImage of a context's colour
 * target. The XR shell gets swapchain handles from OpenXR directly; the
 * pixel test needs this to blit between two renderer contexts. */
void *mr_raw_color_image(mr_context *c);

int main(void) {
    const int S = 16;
    mr_context *c = mr_create(S, S);
    if (!c) { fprintf(stderr, "mr_create failed: %s\n", mr_last_error()); return 1; }

    /* --- Gate 1: clear to a solid colour and read it back --- */
    mr_clear(c, 0xffff0000); /* A=255 R=255 G=0 B=0 -> opaque red */
    uint8_t *fb = malloc((size_t)S * S * 4);
    if (mr_read_framebuffer(c, fb, (size_t)S * S * 4)) {
        fprintf(stderr, "readback 1 failed: %s\n", mr_last_error()); return 1;
    }
    int non_zero = 0;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        uint32_t p = getbgra(fb, x, y, S);
        check("clear", x, y, (int)(p & 0xff), 0);          /* B */
        check("clear", x, y, (int)((p >> 8) & 0xff), 0);   /* G */
        check("clear", x, y, (int)((p >> 16) & 0xff), 255); /* R */
        check("clear", x, y, (int)((p >> 24) & 0xff), 255); /* A */
        if (p) non_zero++;
    }
    if (non_zero != S * S) { fprintf(stderr, "FAIL clear: %d/%d pixels non-zero\n", non_zero, S * S); failures++; }
    else printf("gate1 clear: all %d pixels = red 0x%08x\n", S * S, getbgra(fb, 0, 0, S));

    /* --- Gate 2: textured quad, texel modulated by diffuse --- */
    /* A 1x1 white texel; the shader samples it and multiplies by the
     * per-vertex diffuse colour. Set the quad to a distinct colour so the
     * readback proves the sampler, the descriptor set, and the push
     * constants are all live. */
    uint32_t texel = 0xffffffff; /* BGRA white */
    uint32_t t = mr_texture_create(c, 1, 1, &texel, 4);
    if (!t) { fprintf(stderr, "mr_texture_create failed: %s\n", mr_last_error()); return 1; }

    mr_vertex_rhw q[4] = {0};
    for (int i = 0; i < 4; i++) {
        q[i].x = (i & 1) ? S : 0;
        q[i].y = (i & 2) ? S : 0;
        q[i].z = 0.5f;
        q[i].rhw = 1.0f;
        q[i].color = 0xff00ff00; /* A=255 R=0 G=255 B=0 -> green */
        q[i].u = q[i].v = 0.5f;
    }
    uint16_t ix[6] = {0, 1, 2, 2, 1, 3};

    mr_draw_state s = {0};
    s.texture = t;
    s.blend = MR_BLEND_NONE;      /* opaque */
    s.linear_filter = 1;
    s.address_clamp = 1;
    s.texture_color_only = 0;
    s.texture_alpha_only = 0;
    s.alpha_test_ref = -1;        /* alpha test off */
    s.color_write_mask = 0xF;
    s.depth_enable = 0;
    s.cull_mode = 1;              /* D3DCULL_NONE */

    mr_clear(c, 0xff0000ff);      /* blue background */
    if (mr_draw_rhw(c, &s, MR_TRIANGLE_LIST, q, sizeof q[0], 4, ix, 6)) {
        fprintf(stderr, "draw failed: %s\n", mr_last_error()); return 1;
    }
    if (mr_read_framebuffer(c, fb, (size_t)S * S * 4)) {
        fprintf(stderr, "readback 2 failed: %s\n", mr_last_error()); return 1;
    }
    /* white texel * green diffuse = green; the quad covers the whole 16x16,
     * so the centre must be green, not the blue clear. */
    uint32_t center = getbgra(fb, 8, 8, S);
    check("quad", 8, 8, (int)(center & 0xff), 0);           /* B */
    check("quad", 8, 8, (int)((center >> 8) & 0xff), 255);  /* G */
    check("quad", 8, 8, (int)((center >> 16) & 0xff), 0);   /* R */
    printf("gate2 textured quad: centre = 0x%08x (expect green 0x%08x)\n", center, 0xff00ff00);

    /* --- Gate 3: alpha test discard --- */
    /* Diffuse alpha 128 (0x80), texel alpha 255; alpha-only test keeps the
     * texture alpha. With a threshold of 200, the fragment must be discarded
     * so the blue background survives. */
    mr_draw_state s2 = s;
    for (int i = 0; i < 4; i++) q[i].color = 0x8000ff00; /* alpha 0x80 */
    s2.alpha_test_ref = 200; /* 0x80*255 = 128 < 200 -> discard */
    mr_clear(c, 0xff0000ff);
    if (mr_draw_rhw(c, &s2, MR_TRIANGLE_LIST, q, sizeof q[0], 4, ix, 6)) {
        fprintf(stderr, "alpha-draw failed: %s\n", mr_last_error()); return 1;
    }
    if (mr_read_framebuffer(c, fb, (size_t)S * S * 4)) {
        fprintf(stderr, "readback 3 failed: %s\n", mr_last_error()); return 1;
    }
    uint32_t disc = getbgra(fb, 8, 8, S);
    check("discard", 8, 8, (int)(disc & 0xff), 255);      /* blue survives */
    check("discard", 8, 8, (int)((disc >> 8) & 0xff), 0);
    printf("gate3 alpha discard: centre = 0x%08x (expect blue 0x%08x)\n", disc, 0xff0000ff);

    /* --- Gate 4: cross-size GPU stretch (mr_blit_target_to_scaled) ---
     * The XR shell blits the engine-res source into the FULL eye-res
     * swapchain image: the blit must fill the whole dst extent, not just
     * the src-sized top-left. Here the 16x16 src ctx `c` is the engine
     * target and a 32x32 dst ctx stands in for the swapchain image. */
    const int D = 32;
    mr_context *dst = mr_create(D, D);
    if (!dst) { fprintf(stderr, "dst mr_create failed: %s\n", mr_last_error()); return 1; }

    /* Source: fully red via mr_clear + mr_draw_rhw quad (the whole 16x16 is
     * covered). The blit flushes the source's pending frame itself (the
     * draw path is only on the GPU after the blit's submit), and the pass's
     * finalLayout (COLOR_ATTACHMENT) is exactly what the blit's src barrier
     * assumes. */
    for (int i = 0; i < 4; i++) q[i].color = 0xffff0000; /* red */
    mr_clear(c, 0xff0000ff); /* blue background: any un-stretched corner would stay blue */
    if (mr_draw_rhw(c, &s, MR_TRIANGLE_LIST, q, sizeof q[0], 4, ix, 6)) {
        fprintf(stderr, "stretch src draw failed: %s\n", mr_last_error()); return 1;
    }

    /* Destination: clear blue and SUBMIT it (the readback flushes the
     * dst's own render pass), so after the blit the dst is red iff the
     * blit covered the whole extent, and blue iff it missed. */
    mr_clear(dst, 0xff0000ff);
    uint8_t *fb32 = malloc((size_t)D * D * 4);
    if (mr_read_framebuffer(dst, fb32, (size_t)D * D * 4)) {
        fprintf(stderr, "stretch dst pre-readback failed: %s\n", mr_last_error()); return 1;
    }
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++)
        check("stretch dst pre", x, y, (int)(getbgra(fb32, x, y, D) & 0xff), 255); /* B survives */

    /* The stretch: 16x16 src -> 32x32 dst via the raw handle. */
    if (mr_blit_target_to_scaled(c, mr_raw_color_image(dst), (uint32_t)D, (uint32_t)D)) {
        fprintf(stderr, "stretch blit failed: %s\n", mr_last_error()); return 1;
    }
    if (mr_read_framebuffer(dst, fb32, (size_t)D * D * 4)) {
        fprintf(stderr, "stretch dst readback failed: %s\n", mr_last_error()); return 1;
    }
    int red = 0;
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
        uint32_t p = getbgra(fb32, x, y, D);
        check("stretch", x, y, (int)(p & 0xff), 0);             /* B */
        check("stretch", x, y, (int)((p >> 8) & 0xff), 0);      /* G */
        check("stretch", x, y, (int)((p >> 16) & 0xff), 255);   /* R */
        check("stretch", x, y, (int)((p >> 24) & 0xff), 255);   /* A */
        if (((p >> 16) & 0xff) == 255) red++;
    }
    if (red != D * D) { fprintf(stderr, "FAIL stretch: %d/%d pixels red\n", red, D * D); failures++; }
    else printf("gate4 stretch blit: 16x16 -> 32x32 all %d pixels red (corner 31,31 = 0x%08x)\n",
                D * D, getbgra(fb32, 31, 31, D));
    free(fb32);
    mr_destroy(dst);

    free(fb);
    mr_texture_destroy(c, t);
    mr_destroy(c);
    if (failures) { fprintf(stderr, "VK_PIXEL FAIL (%d checks)\n", failures); return 1; }
    printf("VK_PIXEL PASS\n");
    return 0;
}
