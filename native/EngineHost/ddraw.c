/* Minimal IDirectDraw7 so the game's display probing succeeds (modes, refresh rate, device identity). */
#include "host.h"
#include <stdlib.h>
#define DD_OK 0
typedef struct { const char *name; uint8_t argc; } Method;
static const Method m_dd[] = { {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Compact",1},{"CreateClipper",4},{"CreatePalette",5},{"CreateSurface",4},{"DuplicateSurface",3},{"EnumDisplayModes",5},{"EnumSurfaces",5},{"FlipToGDISurface",1},{"GetCaps",3},{"GetDisplayMode",2},{"GetFourCCCodes",3},{"GetGDISurface",2},{"GetMonitorFrequency",2},{"GetScanLine",2},{"GetVerticalBlankStatus",2},{"Initialize",2},{"RestoreDisplayMode",1},{"SetCooperativeLevel",3},{"SetDisplayMode",6},{"WaitForVerticalBlank",3},{"GetAvailableVidMem",4},{"GetSurfaceFromDC",3},{"RestoreAllSurfaces",1},{"TestCooperativeLevel",1},{"GetDeviceIdentifier",3},{"StartModeTest",4},{"EvaluateMode",3} };
#define N(a) (int)(sizeof(a)/sizeof((a)[0]))
static uint32_t vt_dd, dd_object; static int dd_refs;
static const uint32_t modes[][2] = { {640,480},{800,600},{1024,768},{1280,720},{1280,960},{1600,900},{1920,1080} };
static void fill_desc(uint32_t d, uint32_t w, uint32_t h, uint32_t bpp, uint32_t hz) {
    memset(GPTR(d), 0, 124); S32(d, 124); S32(d + 4, 1 | 2 | 4 | 8 | 0x1000 | 0x40000); S32(d + 8, h); S32(d + 12, w); S32(d + 16, w * bpp / 8); S32(d + 20, hz);
    S32(d + 72, 32); S32(d + 76, 0x40); S32(d + 84, bpp);
    if (bpp == 32) { S32(d + 88, 0xFF0000); S32(d + 92, 0xFF00); S32(d + 96, 0xFF); } else { S32(d + 88, 0xF800); S32(d + 92, 0x7E0); S32(d + 96, 0x1F); }
    S32(d + 104, 0x200);  /* DDSCAPS_PRIMARYSURFACE */
}
static void method_dd(EngineCPU *cpu, int i) {
    switch (i) {
    case 1: RET_STDCALL(++dd_refs, 1); case 2: RET_STDCALL(dd_refs > 1 ? --dd_refs : 0, 1);
    case 8: { uint32_t flags = ARG(1), cb = ARG(4), ctx = ARG(3); (void)flags; uint32_t desc = guest_alloc(124);
        host_log("IDirectDraw7::EnumDisplayModes: reporting %d modes", N(modes));
        for (int m = 0; m < N(modes); m++) { for (int bpp = 16; bpp <= 32; bpp += 16) { fill_desc(desc, modes[m][0], modes[m][1], (uint32_t)bpp, 60); uint32_t args[2] = { desc, ctx }; if (!host_call_guest(cpu, cb, 2, args, 1)) goto done; } }
        done: guest_free(desc); RET_STDCALL(DD_OK, 5); }
    case 11: { uint32_t c = ARG(1); uint32_t size = G32(c); memset(GPTR(c + 4), 0, size - 4); S32(c + 4, 0x00000001u); S32(c + 20, 256u << 20); S32(c + 24, 256u << 20); RET_STDCALL(DD_OK, 3); }
    case 12: fill_desc(ARG(1), 1920, 1080, 32, 60); RET_STDCALL(DD_OK, 2);
    case 13: S32(ARG(1), 0); RET_STDCALL(DD_OK, 3);
    case 15: S32(ARG(1), 60); RET_STDCALL(DD_OK, 2);
    case 16: S32(ARG(1), 0); RET_STDCALL(DD_OK, 2);
    case 17: S32(ARG(1), 1); RET_STDCALL(DD_OK, 2);
    case 23: { if (ARG(2)) S32(ARG(2), 256u << 20); if (ARG(3)) S32(ARG(3), 200u << 20); RET_STDCALL(DD_OK, 4); }
    case 27: { uint32_t id = ARG(1); memset(GPTR(id), 0, 1064); strcpy((char *)GPTR(id), "halo-vision-null"); strcpy((char *)GPTR(id + 512), "Halo Vision Null Display"); S64(id + 1024, UINT64_C(0x0009000F00000000)); S32(id + 1032, 0x10DE); S32(id + 1036, 1); RET_STDCALL(DD_OK, 3); }
    case 6: S32(ARG(2), 0); RET_STDCALL(0x8876024Eu, 4);   /* DDERR_UNSUPPORTED: no surfaces here */
    default: RET_STDCALL(DD_OK, m_dd[i].argc);
    }
}
#define M(i) static void shim_dd_##i(EngineCPU *cpu) { method_dd(cpu, i); }
M(0)M(1)M(2)M(3)M(4)M(5)M(6)M(7)M(8)M(9)M(10)M(11)M(12)M(13)M(14)M(15)M(16)M(17)M(18)M(19)M(20)M(21)M(22)M(23)M(24)M(25)M(26)M(27)M(28)M(29)
static HostShim fns[] = { shim_dd_0,shim_dd_1,shim_dd_2,shim_dd_3,shim_dd_4,shim_dd_5,shim_dd_6,shim_dd_7,shim_dd_8,shim_dd_9,shim_dd_10,shim_dd_11,shim_dd_12,shim_dd_13,shim_dd_14,shim_dd_15,shim_dd_16,shim_dd_17,shim_dd_18,shim_dd_19,shim_dd_20,shim_dd_21,shim_dd_22,shim_dd_23,shim_dd_24,shim_dd_25,shim_dd_26,shim_dd_27,shim_dd_28,shim_dd_29 };
SHIM(DirectDrawCreateEx) {
    if (!vt_dd) { vt_dd = guest_alloc(4 * N(m_dd)); char name[96]; for (int i = 0; i < N(m_dd); i++) { snprintf(name, sizeof name, "IDirectDraw7::%s", m_dd[i].name); S32(vt_dd + 4u * (uint32_t)i, host_proc_address("ddraw.dll", name)); }
        dd_object = guest_alloc(16); S32(dd_object, vt_dd); }
    dd_refs++; S32(ARG(1), dd_object); host_log("DirectDrawCreateEx -> IDirectDraw7 %08X", dd_object); RET_STDCALL(DD_OK, 4); }
SHIM(DirectDrawEnumerateExA) { uint32_t cb = ARG(0), ctx = ARG(1);
    static uint32_t desc, drv; if (!desc) { desc = guest_strdup("Primary Display Driver"); drv = guest_strdup("display"); }
    uint32_t args[5] = { 0, desc, drv, ctx, 0x50001 };
    host_log("DirectDrawEnumerateExA: reporting one display to callback %08X", cb);
    host_call_guest(cpu, cb, 5, args, 1); RET_STDCALL(0, 3); }
static HostShimEntry table[40]; static char names[40][64];
const HostShimEntry *host_shims_ddraw_build(void) {
    int n = 0; table[n++] = (HostShimEntry){ "ddraw.dll", "DirectDrawCreateEx", shim_DirectDrawCreateEx }; table[n++] = (HostShimEntry){ "ddraw.dll", "DirectDrawEnumerateExA", shim_DirectDrawEnumerateExA };
    for (int i = 0; i < N(m_dd); i++) { snprintf(names[n], 64, "IDirectDraw7::%s", m_dd[i].name); table[n] = (HostShimEntry){ "ddraw.dll", names[n], fns[i] }; n++; }
    table[n] = (HostShimEntry){ NULL, NULL, NULL }; return table;
}
