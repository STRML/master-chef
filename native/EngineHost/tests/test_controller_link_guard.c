/* The haptics guard that keeps the DualSense on Bluetooth (gamecontroller_guard.h).
 *
 * Replays the pulse rate that preceded Build74's and Build75's drops (five to
 * nine a second) and checks that the guard paces it to four a second, holds
 * pulses back for the grace period after every connect and while no pad is
 * connected, suspends haptics after two pulse-linked drops within two minutes
 * (a minute, then two, then four), and does not count drops with no pulse in
 * the ten seconds before them. */
#include "gamecontroller_guard.h"
#include <assert.h>
#include <stdio.h>

#define MS 1000000ull
#define S 1000000000ull

int main(void) {
    HostGCGuard g; hostgc_guard_init(&g, 250 * MS, 3 * S);
    uint64_t t = 10 * S;
    assert(hostgc_guard_admit(&g, t) == HOSTGC_GUARD_DISCONNECTED);
    hostgc_guard_connected(&g, t);
    assert(hostgc_guard_admit(&g, t + 1 * S) == HOSTGC_GUARD_GRACE);

    /* Build74's rate: a pulse every 130 ms for ten seconds after the grace period. */
    uint64_t played = 0, last = 0;
    for (uint64_t at = t + 3 * S; at < t + 13 * S; at += 130 * MS)
        if (hostgc_guard_admit(&g, at) == HOSTGC_GUARD_PLAY) { assert(!last || at - last >= 250 * MS); last = at; played++; }
    assert(played >= 36 && played <= 40);          /* four a second, not 7.7 */
    assert(g.dropped_rate > 30);

    /* One drop right after a pulse: counted, not yet suspended. */
    hostgc_guard_disconnected(&g, last + 1 * S, last);
    assert(g.linked_disconnects == 1 && g.suspensions == 0);
    assert(hostgc_guard_admit(&g, last + 2 * S) == HOSTGC_GUARD_DISCONNECTED);
    uint64_t back = last + 4 * S; hostgc_guard_connected(&g, back);
    assert(hostgc_guard_admit(&g, back + 2 * S) == HOSTGC_GUARD_GRACE);
    assert(hostgc_guard_admit(&g, back + 3 * S) == HOSTGC_GUARD_PLAY);
    uint64_t pulse = back + 3 * S;

    /* A second pulse-linked drop within two minutes: haptics off for a minute. */
    uint64_t drop = pulse + 5 * S;
    hostgc_guard_disconnected(&g, drop, pulse);
    assert(g.linked_disconnects == 2 && g.suspensions == 1 && g.suspended_until_ns == drop + 60 * S);
    hostgc_guard_connected(&g, drop + 2 * S);
    assert(hostgc_guard_admit(&g, drop + 30 * S) == HOSTGC_GUARD_SUSPENDED);
    assert(hostgc_guard_admit(&g, drop + 61 * S) == HOSTGC_GUARD_PLAY);

    /* Two more linked drops: two minutes this time, then four. */
    uint64_t p2 = drop + 61 * S;
    hostgc_guard_disconnected(&g, p2 + 1 * S, p2); hostgc_guard_connected(&g, p2 + 2 * S);
    assert(hostgc_guard_admit(&g, p2 + 6 * S) == HOSTGC_GUARD_PLAY);
    hostgc_guard_disconnected(&g, p2 + 7 * S, p2 + 6 * S);
    assert(g.suspensions == 2 && g.suspended_until_ns == p2 + 7 * S + 120 * S);
    uint64_t p3 = p2 + 7 * S + 121 * S;
    hostgc_guard_connected(&g, p3 - 10 * S);
    assert(hostgc_guard_admit(&g, p3) == HOSTGC_GUARD_PLAY);
    hostgc_guard_disconnected(&g, p3 + 1 * S, p3); hostgc_guard_connected(&g, p3 + 2 * S);
    assert(hostgc_guard_admit(&g, p3 + 6 * S) == HOSTGC_GUARD_PLAY);
    hostgc_guard_disconnected(&g, p3 + 7 * S, p3 + 6 * S);
    assert(g.suspensions == 3 && g.suspended_until_ns == p3 + 7 * S + 240 * S);

    /* Drops with no pulse in the ten seconds before them do not count against
     * haptics (Build75's later drops), however many there are. */
    HostGCGuard q; hostgc_guard_init(&q, 250 * MS, 3 * S);
    uint64_t u = 100 * S; hostgc_guard_connected(&q, u);
    assert(hostgc_guard_admit(&q, u + 3 * S) == HOSTGC_GUARD_PLAY);
    for (int n = 0; n < 12; n++) {
        uint64_t at = u + 20 * S + (uint64_t)n * 6 * S;
        hostgc_guard_disconnected(&q, at, u + 3 * S); hostgc_guard_connected(&q, at + 2 * S);
    }
    assert(q.disconnects == 12 && q.linked_disconnects == 0 && q.suspensions == 0);

    /* Two linked drops further apart than two minutes do not suspend. */
    HostGCGuard w; hostgc_guard_init(&w, 250 * MS, 0);
    hostgc_guard_connected(&w, 1 * S);
    assert(hostgc_guard_admit(&w, 2 * S) == HOSTGC_GUARD_PLAY);
    hostgc_guard_disconnected(&w, 3 * S, 2 * S); hostgc_guard_connected(&w, 4 * S);
    assert(hostgc_guard_admit(&w, 200 * S) == HOSTGC_GUARD_PLAY);
    hostgc_guard_disconnected(&w, 201 * S, 200 * S);
    assert(w.linked_disconnects == 2 && w.suspensions == 0);

    printf("PASS: haptics paced to 4/s (%llu of 77 pulses at Build74's rate), grace after connect, "
           "suspension 60/120/240 s after pulse-linked drops, unlinked drops ignored\n", (unsigned long long)played);
    return 0;
}
