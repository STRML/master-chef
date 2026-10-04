/* vkshader_test.c — end-to-end check of the D3D8 -> SPIR-V translation layer
 * against the game's real shader corpus, loaded into a real Vulkan driver
 * (lavapipe) with validation enabled.
 *
 * Build (container):
 *   gcc -O1 -g -std=gnu11 -w -I native/EngineHost \
 *       native/EngineHost/android/vkdev/vkshader_test.c \
 *       native/EngineHost/vkshader.c \
 *       third_party/mojoshader/mojoshader.c third_party/mojoshader/mojoshader_common.c \
 *       third_party/mojoshader/profiles/mojoshader_profile_common.c \
 *       third_party/mojoshader/profiles/mojoshader_profile_spirv.c \
 *       -o /tmp/vkshader_test -lvulkan -lm
 *   (with the same -D flags as vkbuild.sh; see vkbuild.sh for the exact set)
 *
 * Usage: vkshader_test <vs-dir> <ps-dir> [corpus-dir]
 *   vs-dir: *.fxc (vs_1_1); ps-dir: *.bin (ps_1_1/ps_1_4/ps_2_0)
 *   The game root is used only for the Vulkan device creation; the
 *   translation layer is independent of the renderer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vkshader.h"

static int load_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 8 || (sz & 3)) { fclose(fp); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -1; }
    fclose(fp);
    *out = buf; *out_len = (size_t)sz;
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int list_dir(const char *dir, const char *suffix, char ***out) {
    DIR *d = opendir(dir);
    if (!d) { *out = NULL; return 0; }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    size_t slen = strlen(suffix);
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        size_t l = strlen(nm);
        if (l < slen || strcmp(nm + l - slen, suffix)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, (size_t)cap * sizeof *names); }
        char *dup = malloc(l + 1);
        memcpy(dup, nm, l + 1);
        names[n++] = dup;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);
    *out = names;
    return n;
}

int main(int argc, char **argv) {
    const char *vs_dir = argc > 1 ? argv[1] : ".scratch/vk/shaders";
    const char *ps_dir = argc > 2 ? argv[2] : ".scratch/vk/capture";
    /* Explicit link pair (probe's known-good pair): argv[3]=vs argv[4]=ps */
    const char *vs_pair = argc > 3 ? argv[3] : NULL;
    const char *ps_pair = argc > 4 ? argv[4] : NULL;

    /* --- Vulkan device (lavapipe with validation) --- */
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        ici.enabledLayerCount = 0;
        if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "vkCreateInstance failed\n"); return 1; }
    }
    uint32_t gpucount = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(inst, &gpucount, &gpu);
    if (!gpucount) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    float prio = 1.0f;
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(gpu, &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "vkCreateDevice failed\n"); return 1; }

    /* --- translate every VS --- */
    char **vs_files; int nvs = list_dir(vs_dir, ".fxc", &vs_files);
    int vs_ok = 0;
    for (int i = 0; i < nvs; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", vs_dir, vs_files[i]);
        uint8_t *buf; size_t len;
        if (load_file(path, &buf, &len)) { printf("  %s: load fail\n", vs_files[i]); continue; }
        vkshader sh;
        if (vkshader_translate((const uint32_t *)buf, len, 0, &sh) == 0) {
            vs_ok++;
        } else {
            printf("  %s: translate fail: %s\n", vs_files[i], sh.error);
        }
        vkshader_destroy(&sh);
        free(buf);
    }
    printf("VS: %d/%d translated\n", vs_ok, nvs);

    /* --- translate every PS --- */
    char **ps_files; int nps = list_dir(ps_dir, ".bin", &ps_files);
    int ps_ok = 0, ps_fail = 0;
    for (int i = 0; i < nps; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", ps_dir, ps_files[i]);
        uint8_t *buf; size_t len;
        if (load_file(path, &buf, &len)) continue;
        vkshader sh;
        if (vkshader_translate((const uint32_t *)buf, len, 1, &sh) == 0) {
            ps_ok++;
        } else {
            ps_fail++;
        }
        vkshader_destroy(&sh);
        free(buf);
    }
    printf("PS: %d/%d translated (%d rejected)\n", ps_ok, nps, ps_fail);

    /* --- link a real pair and load into lavapipe --- */
    if (nvs == 0 || nps == 0) { printf("FAIL: no corpus\n"); return 1; }
    /* pick a linked pair: use the first VS and first PS that both translate */
    vkshader vs, ps;
    int vs_loaded = 0, ps_loaded = 0;
    if (vs_pair) {
        uint8_t *buf; size_t len;
        if (load_file(vs_pair, &buf, &len) == 0) {
            if (vkshader_translate((const uint32_t *)buf, len, 0, &vs) == 0) {
                printf("link pair VS: %s (uniforms=%d attrs=%d)\n", vs_pair, vs.uniform_count, vs.attribute_count);
                vs_loaded = 1;
            } else printf("  VS %s: %s\n", vs_pair, vs.error);
            free(buf);
        }
    }
    for (int i = 0; i < nvs && !vs_loaded; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", vs_dir, vs_files[i]);
        uint8_t *buf; size_t len;
        if (load_file(path, &buf, &len)) continue;
        if (vkshader_translate((const uint32_t *)buf, len, 0, &vs) == 0) {
            printf("link pair VS: %s (uniforms=%d attrs=%d)\n", vs_files[i], vs.uniform_count, vs.attribute_count);
            vs_loaded = 1;
        } else {
            printf("  VS %s: %s\n", vs_files[i], vs.error);
        }
        free(buf);
    }
    if (ps_pair) {
        uint8_t *buf; size_t len;
        if (load_file(ps_pair, &buf, &len) == 0) {
            if (vkshader_translate((const uint32_t *)buf, len, 1, &ps) == 0) {
                printf("link pair PS: %s (uniforms=%d samplers=%d)\n", ps_pair, ps.uniform_count, ps.sampler_count);
                ps_loaded = 1;
            } else printf("  PS %s: %s\n", ps_pair, ps.error);
            free(buf);
        }
    }
    for (int i = 0; i < nps && !ps_loaded; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", ps_dir, ps_files[i]);
        uint8_t *buf; size_t len;
        if (load_file(path, &buf, &len)) continue;
        if (vkshader_translate((const uint32_t *)buf, len, 1, &ps) == 0) {
            printf("link pair PS: %s (uniforms=%d samplers=%d)\n", ps_files[i], ps.uniform_count, ps.sampler_count);
            ps_loaded = 1;
        } else {
            printf("  PS %s: %s\n", ps_files[i], ps.error);
        }
        free(buf);
    }
    if (!vs_loaded || !ps_loaded) { printf("FAIL: no pair to link\n"); return 1; }

    /* Dump the UNLINKED modules for offline comparison (HALO_SPV_DUMP prefix). */
    if (getenv("HALO_SPV_DUMP")) {
        char dpath[1024];
        snprintf(dpath, sizeof dpath, "%s_unlinked_vs.spv", getenv("HALO_SPV_DUMP"));
        FILE *f = fopen(dpath, "wb");
        if (f) { fwrite(vs.spirv, vkshader_spirv_bytes(&vs), 1, f); fclose(f); }
        snprintf(dpath, sizeof dpath, "%s_unlinked_ps.spv", getenv("HALO_SPV_DUMP"));
        f = fopen(dpath, "wb");
        if (f) { fwrite(ps.spirv, vkshader_spirv_bytes(&ps), 1, f); fclose(f); }
    }
    if (vkshader_link(&vs, &ps, NULL) != 0) {
        printf("FAIL: link: %s\n", vs.error);
        return 1;
    }
    printf("link: OK\n");
    /* Dump the linked modules for offline inspection (HALO_SPV_DUMP prefix). */
    if (getenv("HALO_SPV_DUMP")) {
        char dpath[1024];
        snprintf(dpath, sizeof dpath, "%s_vs.spv", getenv("HALO_SPV_DUMP"));
        FILE *f = fopen(dpath, "wb");
        if (f) { fwrite(vs.spirv, vkshader_spirv_bytes(&vs), 1, f); fclose(f); }
        snprintf(dpath, sizeof dpath, "%s_ps.spv", getenv("HALO_SPV_DUMP"));
        f = fopen(dpath, "wb");
        if (f) { fwrite(ps.spirv, vkshader_spirv_bytes(&ps), 1, f); fclose(f); }
    }

    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule sm_vs, sm_ps;
    smi.codeSize = vkshader_spirv_bytes(&vs); smi.pCode = vs.spirv;
    VkResult r1 = vkCreateShaderModule(dev, &smi, NULL, &sm_vs);
    smi.codeSize = vkshader_spirv_bytes(&ps); smi.pCode = ps.spirv;
    VkResult r2 = vkCreateShaderModule(dev, &smi, NULL, &sm_ps);
    printf("vkCreateShaderModule: vs=%d ps=%d\n", r1, r2);

    /* uniform UBO sizes */
    size_t vs_u = vkshader_uniform_size(&vs), ps_u = vkshader_uniform_size(&ps);
    printf("UBO sizes: VS=%zu PS=%zu\n", vs_u, ps_u);

    int rc = (r1 == VK_SUCCESS && r2 == VK_SUCCESS && vs_ok > 0 && ps_ok > 0) ? 0 : 1;
    if (sm_vs) vkDestroyShaderModule(dev, sm_vs, NULL);
    if (sm_ps) vkDestroyShaderModule(dev, sm_ps, NULL);
    vkshader_destroy(&vs);
    vkshader_destroy(&ps);

    /* --- driver-load sweep: how many translated shaders pass validation? --- */
    if (argc > 4 && !strcmp(argv[4], "--load-sweep")) {
        int vs_load_ok = 0, ps_load_ok = 0;
        for (int i = 0; i < nvs; ++i) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s", vs_dir, vs_files[i]);
            uint8_t *buf; size_t len;
            if (load_file(path, &buf, &len)) continue;
            vkshader sh;
            if (vkshader_translate((const uint32_t *)buf, len, 0, &sh) == 0) {
                VkShaderModule m;
                VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                ci.codeSize = vkshader_spirv_bytes(&sh); ci.pCode = sh.spirv;
                if (vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS) { vs_load_ok++; vkDestroyShaderModule(dev, m, NULL); }
                else fprintf(stderr, "[sweep] VS module load FAIL: %s\n", vs_files[i]);
                fflush(stderr);
                vkshader_destroy(&sh);
            }
            free(buf);
        }
        for (int i = 0; i < nps; ++i) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s", ps_dir, ps_files[i]);
            uint8_t *buf; size_t len;
            if (load_file(path, &buf, &len)) continue;
            vkshader sh;
            if (vkshader_translate((const uint32_t *)buf, len, 1, &sh) == 0) {
                VkShaderModule m;
                VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                ci.codeSize = vkshader_spirv_bytes(&sh); ci.pCode = sh.spirv;
                if (vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS) { ps_load_ok++; vkDestroyShaderModule(dev, m, NULL); }
                else fprintf(stderr, "[sweep] PS module load FAIL: %s\n", ps_files[i]);
                fflush(stderr);
                vkshader_destroy(&sh);
            }
            free(buf);
        }
        printf("driver-load: VS %d/%d OK, PS %d/%d OK\n", vs_load_ok, nvs, ps_load_ok, nps);
    }

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    if (rc == 0) printf("VKSHADER_TEST_PASS vs=%d ps=%d\n", vs_ok, ps_ok);
    else printf("VKSHADER_TEST_FAIL\n");
    return rc;
}
