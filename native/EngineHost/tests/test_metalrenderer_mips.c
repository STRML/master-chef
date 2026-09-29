#include "../metalrenderer.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int draw_checker(mr_context *context, uint32_t texture, uint8_t mip_filter,
                        uint8_t max_anisotropy, uint8_t *pixels, int size) {
    mr_program_state state; memset(&state, 0, sizeof state);
    state.color_write_mask = 15; state.cull_mode = 1; state.alpha_test_ref = -1;
    state.samplers[0].texture = texture; state.samplers[0].type = MR_SAMPLER_2D;
    state.samplers[0].address_u = state.samplers[0].address_v = 1;
    state.samplers[0].min_filter = max_anisotropy > 1 ? 3 : 1;
    state.samplers[0].mag_filter = max_anisotropy > 1 ? 3 : 1;
    state.samplers[0].mip_filter = mip_filter;
    state.samplers[0].max_anisotropy = max_anisotropy;
    state.fixed_stages[0].color_op = 2;       /* D3DTOP_SELECTARG1 */
    state.fixed_stages[0].color_arg1 = 2;     /* D3DTA_TEXTURE */
    state.fixed_stages[0].alpha_op = 2;
    state.fixed_stages[0].alpha_arg1 = 2;
    state.fixed_stages[0].result_arg = 1;     /* D3DTA_CURRENT */
    state.fixed_stages[1].color_op = 1;       /* D3DTOP_DISABLE */
    mr_vertex_fixed_rhw quad[4]; memset(quad, 0, sizeof quad);
    quad[0].x = 0; quad[0].y = 0; quad[0].rhw = 1; quad[0].color = 0xffffffffu;
    quad[1].x = size; quad[1].y = 0; quad[1].rhw = 1; quad[1].color = 0xffffffffu; quad[1].uv[0][0] = 1;
    quad[2].x = 0; quad[2].y = size; quad[2].rhw = 1; quad[2].color = 0xffffffffu; quad[2].uv[0][1] = 1;
    quad[3].x = size; quad[3].y = size; quad[3].rhw = 1; quad[3].color = 0xffffffffu; quad[3].uv[0][0] = quad[3].uv[0][1] = 1;
    mr_clear(context, 0xff000000u);
    int result = mr_draw_fixed_rhw(context, &state, MR_TRIANGLE_STRIP, quad, sizeof quad[0], 4, NULL, 0);
    if (result) { fprintf(stderr, "draw failed: %d %s\n", result, mr_last_error()); return result; }
    if (mr_read_framebuffer(context, pixels, (size_t)size * size * 4u)) {
        fprintf(stderr, "readback failed: %s\n", mr_last_error()); return -1;
    }
    return 0;
}

static void stats(const uint8_t *pixels, int size, double *mean, double *variance) {
    double sum = 0, sum2 = 0; int count = 0;
    for (int y = 4; y < size - 4; ++y) for (int x = 4; x < size - 4; ++x) {
        double value = pixels[((size_t)y * size + x) * 4u];
        sum += value; sum2 += value * value; count++;
    }
    *mean = sum / count; *variance = sum2 / count - *mean * *mean;
}

int main(void) {
    enum { TARGET = 64, EDGE = 256 };
    mr_context *context = mr_create(TARGET, TARGET);
    if (!context) { fprintf(stderr, "create failed: %s\n", mr_last_error()); return 1; }

    uint8_t npot[5 * 12];
    memset(npot, 0x80, sizeof npot);
    uint32_t small = mr_texture_create_cached(context, 0x1001, 3, 5, npot, 12);
    if (!small || mr_cached_texture_bytes(context) != 72 || mr_cached_texture_count(context) != 1) {
        fprintf(stderr, "NPOT mip accounting failed: count=%u bytes=%llu error=%s\n",
                mr_cached_texture_count(context), (unsigned long long)mr_cached_texture_bytes(context), mr_last_error());
        return 1;
    }
    if (mr_texture_find_cached(context, 0x1001) != small || mr_cached_texture_bytes(context) != 72) {
        fprintf(stderr, "cached lookup changed mip accounting\n"); return 1;
    }

    uint8_t *checker = malloc((size_t)EDGE * EDGE * 4u);
    uint8_t *unmipped = malloc((size_t)TARGET * TARGET * 4u);
    uint8_t *mipped = malloc((size_t)TARGET * TARGET * 4u);
    if (!checker || !unmipped || !mipped) return 1;
    for (int y = 0; y < EDGE; ++y) for (int x = 0; x < EDGE; ++x) {
        uint8_t value = ((x ^ y) & 1) ? 255 : 0;
        size_t offset = ((size_t)y * EDGE + x) * 4u;
        checker[offset] = checker[offset + 1] = checker[offset + 2] = value; checker[offset + 3] = 255;
    }
    struct timespec start, finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint32_t texture = mr_texture_create_cached(context, 0x1002, EDGE, EDGE, checker, EDGE * 4u);
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double upload_ms = (finish.tv_sec - start.tv_sec) * 1000.0 + (finish.tv_nsec - start.tv_nsec) / 1000000.0;
    const uint64_t expected = 72 + UINT64_C(349524);
    if (!texture || mr_cached_texture_bytes(context) != expected || mr_cached_texture_count(context) != 2) {
        fprintf(stderr, "mip accounting failed: count=%u bytes=%llu expected=%llu error=%s\n",
                mr_cached_texture_count(context), (unsigned long long)mr_cached_texture_bytes(context),
                (unsigned long long)expected, mr_last_error()); return 1;
    }
    if (draw_checker(context, texture, 0, 1, unmipped, TARGET) ||
        draw_checker(context, texture, 2, 1, mipped, TARGET)) return 1;
    double base_mean, base_variance, mip_mean, mip_variance;
    stats(unmipped, TARGET, &base_mean, &base_variance);
    stats(mipped, TARGET, &mip_mean, &mip_variance);
    printf("unmipped mean=%.3f variance=%.3f\n", base_mean, base_variance);
    printf("mipped   mean=%.3f variance=%.3f\n", mip_mean, mip_variance);
    if (mip_mean < 126 || mip_mean > 129 || mip_variance > 1 || fabs(base_mean - mip_mean) < 40) {
        fprintf(stderr, "mip filtering did not reduce minification aliasing\n"); return 1;
    }

    /* A valid D3D anisotropic tuple must build and render rather than falling
     * back to a globally forced sampler. */
    if (draw_checker(context, texture, 2, 16, mipped, TARGET)) return 1;
    stats(mipped, TARGET, &mip_mean, &mip_variance);
    if (mip_mean < 126 || mip_mean > 129) { fprintf(stderr, "anisotropic sample is invalid\n"); return 1; }

    printf("cached bytes=%llu (base texture overhead %.3fx)\n",
           (unsigned long long)mr_cached_texture_bytes(context), 349524.0 / (EDGE * EDGE * 4.0));
    printf("256x256 base upload plus CPU mip generation %.3f ms\n", upload_ms);
    printf("RESULT: mip image, sampler, and memory-accounting checks passed\n");
    free(checker); free(unmipped); free(mipped); mr_destroy(context); return 0;
}
