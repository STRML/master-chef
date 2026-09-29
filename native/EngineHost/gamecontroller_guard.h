/* Keeping the DualSense on Bluetooth while it plays haptics.
 *
 * Headset sessions from Build74 on lost the controller again and again: six
 * drops in Build74's four minutes, fifteen in Build75's, the last two minutes
 * of it every five to seven seconds, so the owner could not play a level
 * through. Build73, with the same controller code, lost it none in five
 * minutes. What changed was how hard the haptics drove it: Build73 asked for
 * about 0.6 pulses a second, Build74 widened the player's firing window and
 * asked for five to eight a second, and every Build74 drop, like Build75's
 * first three, came after 20 to 44 pulses in the five seconds before it. The
 * pulses also went out from the presenter's 90 Hz thread, one Core Haptics
 * player each, on an engine that never shut down and was never released when
 * the pad went away, so after the early drops Build75 kept cycling with no
 * pulses at all.
 *
 * This decides, without touching the hardware, whether a pulse may go out:
 *  - no closer together than min_interval (four a second by default);
 *  - not until the pad has stayed connected for grace, after every connect;
 *  - after two drops within strike_window that each came within link_window of
 *    a pulse, not for a suspension that doubles each time (a minute, then two,
 *    four...), so haptics give way before the link does.
 * The caller serialises access. Times are nanoseconds on one monotonic clock;
 * 0 means never. */
#ifndef HOST_GAMECONTROLLER_GUARD_H
#define HOST_GAMECONTROLLER_GUARD_H
#include <stdint.h>

typedef struct {
    uint64_t min_interval_ns, grace_ns, link_window_ns, strike_window_ns, base_suspend_ns;
    uint64_t last_pulse_ns;       /* last pulse admitted */
    uint64_t connected_at_ns;     /* last connect; 0 while none is connected */
    uint64_t suspended_until_ns;
    uint64_t strike_ns[2];        /* the two most recent pulse-linked drops */
    uint64_t next_suspend_ns;
    /* Counters for the device report. */
    uint64_t admitted, dropped_rate, dropped_grace, dropped_suspended, dropped_disconnected;
    uint64_t connects, disconnects, linked_disconnects, suspensions;
} HostGCGuard;

enum { HOSTGC_GUARD_PLAY = 0, HOSTGC_GUARD_RATE, HOSTGC_GUARD_GRACE, HOSTGC_GUARD_SUSPENDED, HOSTGC_GUARD_DISCONNECTED };

static inline void hostgc_guard_init(HostGCGuard *g, uint64_t min_interval_ns, uint64_t grace_ns) {
    *g = (HostGCGuard){0};
    g->min_interval_ns = min_interval_ns;
    g->grace_ns = grace_ns;
    g->link_window_ns = 10000000000ull;      /* a drop within 10 s of a pulse counts against haptics */
    g->strike_window_ns = 120000000000ull;   /* two such drops within two minutes */
    g->base_suspend_ns = 60000000000ull;     /* suspend for a minute, doubling */
}

static inline void hostgc_guard_connected(HostGCGuard *g, uint64_t now) {
    g->connects++;
    g->connected_at_ns = now ? now : 1;
}

/* last_played_ns: when the hardware last actually played a pulse (0 = never). */
static inline void hostgc_guard_disconnected(HostGCGuard *g, uint64_t now, uint64_t last_played_ns) {
    g->disconnects++;
    g->connected_at_ns = 0;
    if (!last_played_ns || now < last_played_ns || now - last_played_ns > g->link_window_ns) return;
    g->linked_disconnects++;
    g->strike_ns[0] = g->strike_ns[1];
    g->strike_ns[1] = now;
    if (g->strike_ns[0] && now - g->strike_ns[0] <= g->strike_window_ns) {
        uint64_t length = g->next_suspend_ns ? g->next_suspend_ns : g->base_suspend_ns;
        g->suspended_until_ns = now + length;
        g->next_suspend_ns = length * 2;
        g->suspensions++;
        g->strike_ns[0] = g->strike_ns[1] = 0;
    }
}

static inline int hostgc_guard_admit(HostGCGuard *g, uint64_t now) {
    if (!g->connected_at_ns) { g->dropped_disconnected++; return HOSTGC_GUARD_DISCONNECTED; }
    if (now < g->suspended_until_ns) { g->dropped_suspended++; return HOSTGC_GUARD_SUSPENDED; }
    if (now - g->connected_at_ns < g->grace_ns) { g->dropped_grace++; return HOSTGC_GUARD_GRACE; }
    if (g->last_pulse_ns && now - g->last_pulse_ns < g->min_interval_ns) { g->dropped_rate++; return HOSTGC_GUARD_RATE; }
    g->last_pulse_ns = now;
    g->admitted++;
    return HOSTGC_GUARD_PLAY;
}

#endif
