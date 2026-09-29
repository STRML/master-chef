/* Optional source-input configuration for the host's enumerated gamepad.
 *
 * A fresh PC profile recognizes "Halo Vision Gamepad" but has no known-device
 * preset: its axis/button bindings are 0x7fff, and the device may be unassigned.
 * HALO_A10_GAMEPAD=1 supplies a session-only preset at the original main-loop
 * boundary. The original DirectInput poll, action mapper, player input and
 * simulation continue to consume the actual bridge snapshot. No poses, player
 * commands, script gates or profile files are changed here.
 *
 * Source layout, checked against generated instructions:
 * 0053B620: device->slot at 006B1A98+device*0x240, reverse at006B2CE8+slot*4.
 * 0048B7B0: binding descriptor {u16 kind,slot,objectKind,index;u32 direction};
 * kind3/objectKind1/dir1 writes positive axis00710536+(slot*32+axis)*4;
 * dir2 writes negative00710538+(slot*32+axis)*4. objectKind0 writes button
 * 00710426+(slot*32+button)*2. 0048CCA0 reads accept/back button indices at
 * 00710526/00710528 and accumulates the original00712498 action state.
 * IDs come from original action names0065B730 (16-byte records).
 */
#include <stdlib.h>
#include "gamecontroller.h"

static float a10_gamepad_float(uint32_t address) {
    float value;
    memcpy(&value, GPTR(address), sizeof value);
    return value;
}

static unsigned a10_gamepad_bind_if_empty(uint32_t address, uint16_t action) {
    if (G16(address) != 0x7fffu) return 0;
    S16(address, action);
    return 1;
}

static void host_a10_gamepad_boundary(EngineCPU *cpu, uint32_t address) {
    static int enabled = -1, trace;
    static uint32_t loops;
    (void)cpu;
    if (enabled < 0) {
        const char *option = getenv("HALO_A10_GAMEPAD");
        enabled = option && !strcmp(option, "1");
        trace = getenv("HALO_A10_TRACE") != NULL;
    }
    if (!enabled || address != 0x004C6E80u) return;
    if (G16(0x00719720u) != 0 || G8(0x00718FC9u) || G8(0x00719739u) ||
        strcmp(GSTR(0x00719779u), "levels\\a10\\a10")) return;
    uint32_t players = G32(0x0087A478u);
    if (!players || G32(players + 4) == UINT32_MAX ||
        G32(0x006B1844u) < 1 || G32(0x006B1A74u) != 0x6F1D2B70u ||
        !hostgc_connected()) return;

    /* Only claim the free bridge/device-zero pair. Preserve any other owner's
     * assignment and existing nonempty custom bindings. */
    uint32_t device_slot = G32(0x006B1A98u), slot_device = G32(0x006B2CE8u);
    unsigned changed = 0;
    if (device_slot == UINT32_MAX && slot_device == UINT32_MAX) {
        S32(0x006B1A98u, 0);
        S32(0x006B2CE8u, 0);
        changed += 2;
    } else if (device_slot != 0 || slot_device != 0) {
        return;
    }

    /* The host exposes X,Y,Z,Rx,Ry,Rz; its Y axes invert GC's upward sign.
     * Source axis range is[-4096,+4096], established by00491C50. Spare trigger
     * axes stay unbound; their digital buttons provide fire/zoom. */
    static const struct { uint16_t index, positive, negative; } axes[] = {
        {0, 22, 21}, /* X: right / left */
        {1, 20, 19}, /* Y: backward / forward */
        {3, 26, 25}, /* Rx: look right / look left */
        {4, 24, 23}, /* Ry: look down / look up */
    };
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        changed += a10_gamepad_bind_if_empty(0x00710536u + 4u * axes[i].index, axes[i].positive);
        changed += a10_gamepad_bind_if_empty(0x00710538u + 4u * axes[i].index, axes[i].negative);
    }
    static const struct { uint16_t button, action; } buttons[] = {
        {HOSTGC_BTN_A, 0},               /* jump; also accept below */
        {HOSTGC_BTN_B, 9},               /* back; also cinematic skip below */
        {HOSTGC_BTN_X, 2},               /* action/use */
        {HOSTGC_BTN_Y, 3},               /* switch weapon */
        {HOSTGC_BTN_LSHOULDER, 6},       /* throw grenade */
        {HOSTGC_BTN_RSHOULDER, 4},       /* melee */
        {HOSTGC_BTN_LTRIGGER, 11},       /* zoom */
        {HOSTGC_BTN_RTRIGGER, 7},        /* fire */
        {HOSTGC_BTN_LTHUMB, 10},         /* crouch */
        {HOSTGC_BTN_RTHUMB, 11},         /* zoom */
        {HOSTGC_BTN_OPTIONS, 12},        /* scoreboard */
        {HOSTGC_BTN_DPAD_UP, 5},         /* flashlight */
        {HOSTGC_BTN_DPAD_DOWN, 1},       /* switch grenade */
        {HOSTGC_BTN_DPAD_LEFT, 13},      /* reload */
        {HOSTGC_BTN_DPAD_RIGHT, 14},     /* exchange weapon */
    };
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
        changed += a10_gamepad_bind_if_empty(0x00710426u + 2u * buttons[i].button, buttons[i].action);
    if (G16(0x00710526u) == 0xffffu) { S16(0x00710526u, HOSTGC_BTN_A); changed++; }
    if (G16(0x00710528u) == 0xffffu) { S16(0x00710528u, HOSTGC_BTN_B); changed++; }

    if (changed)
        host_log("[a10-pad] configured %u empty live fields: device0->slot0; left stick move, right stick look; original input pipeline", changed);
    loops++;
    if (trace && (loops <= 3 || loops % 60 == 0))
        host_log("[a10-pad] loop=%u assigned=%u/%u axes=%d,%d,%d,%d,%d,%d aggregate=%.5f,%.5f,%.5f,%.5f analogLook=%u accept=%u back=%u",
            loops, G32(0x006B1A98u), G32(0x006B2CE8u),
            (int16_t)G16(0x006B2A88u), (int16_t)G16(0x006B2A8Au),
            (int16_t)G16(0x006B2A8Cu), (int16_t)G16(0x006B2A8Eu),
            (int16_t)G16(0x006B2A90u), (int16_t)G16(0x006B2A92u),
            a10_gamepad_float(0x007124ACu), a10_gamepad_float(0x007124B0u),
            a10_gamepad_float(0x007124B4u), a10_gamepad_float(0x007124B8u),
            G8(0x007124BCu), G8(0x007124A0u), G8(0x007124A1u));
}
