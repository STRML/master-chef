/* Production heap, no translated code/game assets. Build from native/EngineHost:
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_guest_heap.c -Wl,-dead_strip -o /tmp/test_guest_heap
 * Replace -O2 with -O1 -g -fsanitize=address,undefined or -fsanitize=thread. */
#include <stdlib.h>
static int fail_native_allocation;
static void *test_calloc(size_t n, size_t size) { return fail_native_allocation ? NULL : calloc(n, size); }
#define calloc test_calloc
#include "../host.c"
#undef calloc
#include <assert.h>
_Thread_local EngineCPU *host_active_cpu;

static void expect_allocation_failure(uint32_t bytes) {
    host_is_main_context = 1; host_escape_ready = 1;
    int how = setjmp(host_escape);
    if (!how) { guest_alloc(bytes); assert(!"allocation should fail"); }
    assert(how == 2 && host_exit_code == 3);
    host_escape_ready = 0;
    assert(!pthread_mutex_trylock(&heap_lock));
    assert(!pthread_mutex_unlock(&heap_lock));
}
static void check_empty(void) {
    assert(heap_live_blocks == 0 && heap_live_bytes == 0);
    assert(heap_initial.base == HEAP_START && heap_initial.span == HEAP_END - HEAP_START);
    assert(!heap_initial.live && !heap_initial.prev && !heap_initial.next);
    for (unsigned i = 0; i < HEAP_HASH_COUNT; i++) assert(!heap_hash[i]);
    for (unsigned i = 0; i < HEAP_BIN_COUNT; i++)
        assert(heap_bins[i] == (i == heap_bin(HEAP_END - HEAP_START) ? &heap_initial : NULL));
}
static void assert_zero(uint32_t p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) assert(G8(p + i) == 0);
}
static uint32_t random_next(uint32_t *state) {
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}
static void *worker(void *opaque) {
    uint32_t seed = (uint32_t)(uintptr_t)opaque, slots[32] = {0}, sizes[32] = {0};
    for (unsigned i = 0; i < 12000; i++) {
        unsigned slot = random_next(&seed) % 32;
        if (slots[slot]) {
            assert(guest_alloc_size(slots[slot]) == sizes[slot]);
            assert(G32(slots[slot]) == slot + 1 && G32(slots[slot] + sizes[slot] - 4) == slot + 1);
            guest_free(slots[slot]);
        }
        uint32_t n = 8 + random_next(&seed) % 32768;
        uint32_t p = guest_alloc(n);
        assert(!(p & 15u) && guest_alloc_size(p) == n);
        assert_zero(p, (n + 15u) & ~15u);
        S32(p, slot + 1); S32(p + n - 4, slot + 1);
        slots[slot] = p; sizes[slot] = n;
        if (!(i % 32)) {
            uint64_t stats[6]; host_heap_stats(stats);
            assert(stats[0] >= n && stats[1] >= stats[0] && stats[2] >= stats[1]);
            assert(stats[3] >= 1 && stats[3] <= 128 && stats[4] > 0);
            assert(stats[5] <= HEAP_END - HEAP_START);
            sched_yield();
        }
    }
    for (unsigned i = 0; i < 32; i++) if (slots[i]) guest_free(slots[i]);
    return NULL;
}
int main(void) {
    engine_flat_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(engine_flat_base != MAP_FAILED);
    /* Failed native metadata allocation changes no ownership and unlocks. */
    fail_native_allocation = 1; expect_allocation_failure(16); check_empty();
    assert(heap_slab_count == 0); fail_native_allocation = 0;
    /* Zero-size blocks own distinct valid payloads; retain original window/prefix. */
    uint32_t a = guest_alloc(0), b = guest_alloc(1), c = guest_alloc(17);
    assert(a == HEAP_START + 16 && b == a + 32 && c == b + 32);
    assert(guest_alloc_size(a) == 0 && guest_alloc_size(b) == 1 && guest_alloc_size(c) == 17);
    assert(G32(c - 4) == 17); assert_zero(c, 32);
    memset(GPTR(c - 16), 0xf7, 48); /* corrupted guest prefix is not allocator metadata */
    assert(guest_alloc_size(c) == 17);
    S32(c + 12, UINT32_MAX); /* forged interior size header */
    const uint32_t invalid[] = {0, 1, HEAP_START, HEAP_END, PAGE_START, UINT32_MAX, c - 16, c + 1, c + 16};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        assert(guest_alloc_size(invalid[i]) == 0); guest_free(invalid[i]);
    }
    assert(heap_live_blocks == 3 && guest_alloc_size(c) == 17);
    guest_free(b); guest_free(b); assert(guest_alloc_size(b) == 0);
    uint32_t d = guest_alloc(1); assert(d == b); assert_zero(d, 16);
    guest_free(a); guest_free(c); guest_free(d); check_empty();
    /* Split recycled storage and coalesce; clear old headers and tiny tails. */
    a = guest_alloc(160); b = guest_alloc(16);
    memset(GPTR(a), 0x93, 160); guest_free(a);
    c = guest_alloc(32); d = guest_alloc(96);
    assert(c == a && d == a + 48); assert_zero(c, 32); assert_zero(d, 112);
    guest_free(d); guest_free(c); guest_free(b); check_empty();
    char text[] = "guest aliases share one owned allocation";
    a = guest_strdup(text); uint32_t alias = a;
    assert(!strcmp(GSTR(alias), text)); S8(alias, 'G'); assert(G8(a) == 'G');
    guest_free(a); assert(guest_alloc_size(alias) == 0); check_empty();
    expect_allocation_failure(UINT32_MAX); expect_allocation_failure(0x20000001u);
    /* Mixed sizes exceed 992 MiB cumulatively; nodes plateau with peak live set. */
    uint32_t live[64] = {0}, seed = 0x1936572u;
    uint64_t turnover = 0;
    for (unsigned cycle = 0; cycle < 4096; cycle++) {
        for (unsigned i = 0; i < 64; i++) {
            if (live[i]) {
                uint32_t old_size = guest_alloc_size(live[i]);
                assert(old_size && G8(live[i]) == 0x5a && G8(live[i] + old_size - 1) == 0xc3);
                guest_free(live[i]);
            }
            uint32_t n = 16384 + random_next(&seed) % 32768;
            live[i] = guest_alloc(n); assert(guest_alloc_size(live[i]) == n);
            assert(G8(live[i]) == 0 && G8(live[i] + n - 1) == 0);
            S8(live[i], 0x5a); S8(live[i] + n - 1, 0xc3); turnover += n;
        }
    }
    assert(turnover > UINT64_C(8) * 1024 * 1024 * 1024);
    for (unsigned i = 0; i < 64; i++) guest_free(live[i]);
    check_empty(); assert(heap_slab_count == 1);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1)));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    check_empty(); assert(heap_slab_count <= 2);
    /* Real window exhaustion still produces status 3, unlocks, and never wraps. */
    a = guest_alloc(0x20000000u);
    b = guest_alloc((HEAP_END - HEAP_START) - 0x20000000u - 32u);
    assert(b + guest_alloc_size(b) == HEAP_END);
    expect_allocation_failure(1);
    guest_free(a); c = guest_alloc(16); assert(c == a); guest_free(c); guest_free(b);
    check_empty();
    uint64_t stats[6]; host_heap_stats(stats);
    assert(stats[0] == 0 && stats[1] == HEAP_END - HEAP_START - 32u);
    assert(stats[2] > turnover && stats[3] == 0 && stats[5] == HEAP_END - HEAP_START);
    assert(stats[4] == sizeof heap_initial + sizeof heap_bins + sizeof heap_hash +
                       (uint64_t)heap_slab_count * sizeof(HeapSlab));
    assert(!munmap(engine_flat_base, GUEST_SIZE));
    printf("PASS: heap exact ownership, corrupted headers, split/coalesce, zeroed reuse, mixed turnover (%llu bytes), bounded metadata, exhaustion and concurrent churn.\n", (unsigned long long)turnover);
}
