/* vkshader_diag.c — find which PS shaders fail to load as linked pairs.
 *
 * For each PS: translate a fresh VS, link the pair, create a VkShaderModule,
 * and print the filename on failure.
 *
 * Build (container): see vkshader_test.c header for the gcc line; swap in
 * this file instead of vkshader_test.c.
 * Usage: vkshader_diag <vs-dir> <ps-dir>
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

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char **)a, *(const char **)b); }

static int list_dir(const char *dir, const char *suffix, char ***out) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **files = NULL; int n = 0;
    size_t sl = strlen(suffix);
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l <= sl || strcmp(de->d_name + l - sl, suffix)) continue;
        files = realloc(files, (n + 1) * sizeof *files);
        files[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(files, n, sizeof *files, cmp_str);
    *out = files;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <vs-dir> <ps-dir>\n", argv[0]); return 2; }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers;
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no instance\n"); return 1; }
    uint32_t ng = 1;
    VkPhysicalDevice gpus[1];
    vkEnumeratePhysicalDevices(inst, &ng, gpus);
    if (!ng) { fprintf(stderr, "no gpu\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    if (vkCreateDevice(gpus[0], &dci, NULL, &dev) != VK_SUCCESS) { fprintf(stderr, "no device\n"); return 1; }

    char **vs_files; int nvs = list_dir(argv[1], ".fxc", &vs_files);
    char **ps_files; int nps = list_dir(argv[2], ".bin", &ps_files);

    int ok = 0, bad = 0;
    for (int i = 0; i < nps; ++i) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[2], ps_files[i]);
        uint8_t *buf; size_t len;
        if (load_file(path, &buf, &len)) continue;
        vkshader ps;
        if (vkshader_translate((const uint32_t *)buf, len, 1, &ps) != 0) {
            printf("REJECT %s: %s\n", ps_files[i], ps.error);
            vkshader_destroy(&ps); free(buf);
            continue;
        }
        /* link against the first VS that translates */
        vkshader vs; int vs_ok = 0;
        for (int j = 0; j < nvs; ++j) {
            char vpath[1024]; snprintf(vpath, sizeof vpath, "%s/%s", argv[1], vs_files[j]);
            uint8_t *vbuf; size_t vlen;
            if (load_file(vpath, &vbuf, &vlen)) continue;
            if (vkshader_translate((const uint32_t *)vbuf, vlen, 0, &vs) == 0) {
                if (vkshader_link(&vs, &ps, NULL) == 0) { vs_ok = 1; }
                free(vbuf);
                if (vs_ok) break;
                vkshader_destroy(&vs);
            } else free(vbuf);
        }
        if (!vs_ok) { printf("NOLINK %s\n", ps_files[i]); vkshader_destroy(&ps); free(buf); continue; }
        /* VS module */
        VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = vkshader_spirv_bytes(&vs); smi.pCode = vs.spirv;
        VkShaderModule mv;
        if (vkCreateShaderModule(dev, &smi, NULL, &mv) != VK_SUCCESS) {
            printf("BADVS %s\n", ps_files[i]);
        } else vkDestroyShaderModule(dev, mv, NULL);
        /* PS module */
        smi.codeSize = vkshader_spirv_bytes(&ps); smi.pCode = ps.spirv;
        VkShaderModule mp;
        if (vkCreateShaderModule(dev, &smi, NULL, &mp) == VK_SUCCESS) {
            ok++; vkDestroyShaderModule(dev, mp, NULL);
        } else {
            bad++; printf("BADPS %s\n", ps_files[i]);
        }
        vkshader_destroy(&vs);
        vkshader_destroy(&ps);
        free(buf);
    }
    printf("linked-pair: PS %d/%d OK, %d bad\n", ok, nps, bad);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
