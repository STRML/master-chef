#ifndef HALO_PANORAMA_OVERLAY_SCOPE_H
#define HALO_PANORAMA_OVERLAY_SCOPE_H

#include <stdint.h>

/* Original Halo PC 1.10 camera-relative overlay entries. */
static inline int halo_panorama_overlay_entry(uint32_t address) {
    return address == 0x004924B0u || /* First-person weapon and hands renderer */
           address == 0x00494730u || /* HUD */
           address == 0x004984C0u;   /* UI screens */
}

/* The game's interface record. 004C9260 appends one record after the player
 * views (word -1, byte +2 set), and 0050BEA0 hands it to 0050BDC0 with EAX 0
 * (0050BF2E xor eax,eax). That call draws nothing but 2D on top of the frame:
 * the cinematic letterbox and chapter titles (004499C0), the game timer
 * (004ADD10), the error and loading modal (00497410), 00496730's messages and
 * the framerate counter (00512530/00512E80). It clears nothing: 005175C0
 * clears only when byte +5 of its record is clear, and 0050BDC0 sets it for
 * EAX 0. EAX 1 is a player view with no camera (0050BF7B), which clears the
 * world target and stands in for the world, so it stays a world view. */
static inline int halo_panorama_interface_entry(uint32_t address, uint32_t eax) {
    return address == 0x0050BDC0u && eax == 0;
}

#endif
