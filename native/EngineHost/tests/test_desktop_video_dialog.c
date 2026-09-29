/* Focused USER32/resource regression for Halo's startup video-mode check.
 * Includes the production translation units so the fixture cannot drift from
 * the actual shim and dialog parser. No translated engine or GPU is started. */
#include "../shims_misc.c"
#include "../resources.c"
#include <assert.h>

uint8_t *engine_flat_base;
const char *host_game_root = ".";
const uint32_t engine_pe_image_base = 0x00400000u;
uint32_t host_main_hwnd = 0x00010001u;
enum { TEST_STRINGS_MODULE = 0x00020000u };
const char *host_module_name(uint32_t handle) {
    return handle == TEST_STRINGS_MODULE ? "strings.dll" : NULL;
}

static void invoke(HostShim shim, EngineCPU *cpu, const uint32_t *args, uint32_t count) {
    cpu->gpr[4] = 0x1000;
    S32(cpu->gpr[4], 0xDEADC0DEu);
    for (uint32_t i = 0; i < count; ++i) S32(cpu->gpr[4] + 4u + 4u*i, args[i]);
    shim(cpu);
    assert(cpu->pc == 0xDEADC0DEu);
    assert(cpu->gpr[4] == 0x1004u + 4u*count);
}

static void expect_rect(uint32_t address, uint32_t width, uint32_t height) {
    assert(G32(address) == 0 && G32(address + 4) == 0);
    assert(G32(address + 8) == width && G32(address + 12) == height);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    host_game_root = argv[1];
    engine_flat_base = calloc(0x10000, 1);
    assert(engine_flat_base);
    EngineCPU cpu = {0};

    /* Model a 1280x960 game client inside the 1920x1080 virtual adapter. */
    screen_w = 1280; screen_h = 960;
    invoke(shim_GetDesktopWindow, &cpu, NULL, 0);
    uint32_t desktop = cpu.gpr[0];
    assert(desktop == HOST_DESKTOP_HWND && desktop != host_main_hwnd);

    uint32_t desktop_args[2] = { desktop, 0x2000 };
    invoke(shim_GetWindowRect, &cpu, desktop_args, 2);
    assert(cpu.gpr[0] == 1); expect_rect(0x2000, 1920, 1080);

    uint32_t client_args[2] = { host_main_hwnd, 0x2020 };
    invoke(shim_GetClientRect, &cpu, client_args, 2);
    assert(cpu.gpr[0] == 1); expect_rect(0x2020, 1280, 960);
    client_args[1] = 0x2040;
    invoke(shim_GetWindowRect, &cpu, client_args, 2);
    expect_rect(0x2040, 1280, 960);

    uint32_t metric = 0;
    invoke(shim_GetSystemMetrics, &cpu, &metric, 1); assert(cpu.gpr[0] == 1920);
    metric = 1;
    invoke(shim_GetSystemMetrics, &cpu, &metric, 1); assert(cpu.gpr[0] == 1080);
    uint32_t device_caps[2] = { 0x20001, 8 };  /* HORZRES */
    invoke(shim_GetDeviceCaps, &cpu, device_caps, 2); assert(cpu.gpr[0] == 1920);
    device_caps[1] = 10;                       /* VERTRES */
    invoke(shim_GetDeviceCaps, &cpu, device_caps, 2); assert(cpu.gpr[0] == 1080);

    char summary[1024];
    uint32_t choice = host_dialog_choose(TEST_STRINGS_MODULE, 102, summary, sizeof summary);
    assert(strstr(summary, "Continue Anyway"));
    assert(strstr(summary, "Safe Mode"));
    assert(choice == 1004);
    printf("DESKTOP_CLIENT_DISTINCTION_PASS desktop=1920x1080 client=1280x960\n");
    printf("NORMAL_VIDEO_DIALOG_PASS choice=%u summary=%s\n", choice, summary);
    free(engine_flat_base);
    return 0;
}
