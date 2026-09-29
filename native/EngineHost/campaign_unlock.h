#ifndef HALO_CAMPAIGN_UNLOCK_H
#define HALO_CAMPAIGN_UNLOCK_H
#include <stdint.h>
/* Retail 1.10 profile_unlock_solo_levels, original instructions 00481400..1C.
 * Only the menu's ten difficulty masks and unlock flag change. The original
 * callback's disk-save and HSC return are deliberately not invoked here. */
static inline void halo_unlock_campaign_menu(uint8_t *profile) {
    profile[0x11c] |= 4;
    for (unsigned i = 0; i < 10; ++i) profile[0x11e + i] |= 15;
}
#endif
