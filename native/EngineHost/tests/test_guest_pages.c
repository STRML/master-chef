/* Production guest page allocator. Build from native/EngineHost:
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_guest_pages.c -Wl,-dead_strip -o /tmp/test_guest_pages
 * The unused host runtime is stripped; no translated/game code is linked. */
#include "../host.c"
#include <assert.h>

static void expect_allocation_failure(uint32_t bytes) {
    host_is_main_context = 1;
    host_escape_ready = 1;
    int how = setjmp(host_escape);
    if (!how) {
        guest_page_alloc(bytes);
        assert(!"allocation should fail");
    }
    assert(how == 2 && host_exit_code == 3);
    host_escape_ready = 0;
}

static void *allocate_worker(void *opaque) {
    uint32_t value = (uint32_t)(uintptr_t)opaque;
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t p = guest_page_alloc(70000);
        assert((p & 0xffffu) == 0 && G32(p) == 0 && G32(p + 131068) == 0);
        S32(p, value);
        S32(p + 131068, value);
        sched_yield();
        assert(G32(p) == value && G32(p + 131068) == value);
        assert(guest_page_free(p));
    }
    return NULL;
}

int main(void) {
    engine_flat_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(engine_flat_base != MAP_FAILED);
    assert(guest_page_alloc(0) == 0);

    /* Exact ownership: an interior/alignment/duplicate free must never expose
     * any part of a live allocation to a second caller. */
    uint32_t a = guest_page_alloc(65537), b = guest_page_alloc(1), c = guest_page_alloc(1);
    assert(a == PAGE_START && b == a + 131072 && c == b + 65536);
    memset(GPTR(a), 0xa5, 131072);
    assert(guest_page_reserve_explicit(a + 65536, 1) == a + 65536);
    assert(!guest_page_reserve_explicit(a, 3 * 65536));
    assert(!guest_page_reserve_explicit(HEAP_END, a - HEAP_END + 1));
    assert(!guest_page_free(a + 1));
    assert(!guest_page_free(a + 65536));
    assert(!guest_page_free(PAGE_START - 65536));
    assert(!guest_page_free(PAGE_END));
    assert(guest_page_free(a));
    assert(!guest_page_free(a));
    assert(guest_page_alloc(65537) == a);
    for (unsigned i = 0; i < 131072; i++) assert(G8(a + i) == 0);
    assert(guest_page_free(a));
    assert(guest_page_free(b));
    assert(guest_page_alloc(3 * 65536) == a); /* adjacent freed ranges coalesce */
    assert(guest_page_free(a));
    assert(guest_page_free(c));

    /* Reserve a game pool spanning the anonymous range; it cannot be returned
     * through resource release. Recommitting an allocated range keeps ownership. */
    a = guest_page_alloc(2 * 65536);
    S32(a, 0xa5a5a5a5);
    S32(a + 2 * 65536 - 4, 0xb6b6b6b6);
    assert(guest_page_free(a));
    assert(guest_page_reserve_explicit(HEAP_END, 0x10020000) == HEAP_END);
    assert(G32(a) == 0 && G32(a + 2 * 65536 - 4) == 0);
    S32(a, 0x98765432);
    assert(guest_page_reserve_explicit(a, 1) == a && G32(a) == 0x98765432);
    assert(!guest_page_free(PAGE_START));
    a = guest_page_alloc(1);
    assert(a == PAGE_START + 2 * 65536);
    S32(a, 0x12345678);
    assert(guest_page_reserve_explicit(a, 1) == a && G32(a) == 0x12345678);
    assert(guest_page_free(a));
    assert(!guest_page_reserve_explicit(PAGE_END - 65536, UINT32_MAX));
    assert(!guest_page_reserve_explicit(HEAP_END, 0));
    assert(!guest_page_reserve_explicit(HEAP_END - 1, 1));
    expect_allocation_failure(UINT32_MAX); /* rounded size cannot wrap */
    expect_allocation_failure(PAGE_END - PAGE_START); /* reserved pages are unavailable */

    /* Exceed the old 752 MiB lifetime limit many times with a bounded live set. */
    for (unsigned i = 0; i < 4096; i++) {
        a = guest_page_alloc(1024 * 1024);
        assert(a == PAGE_START + 2 * 65536 && G32(a) == 0);
        S32(a, i + 1);
        assert(guest_page_free(a));
    }
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(workers + i, NULL, allocate_worker, (void *)(uintptr_t)(i + 1)));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
    for (uint32_t i = 2; i < PAGE_COUNT; i++) assert(page_allocations[i] == 0);
    assert(!munmap(engine_flat_base, GUEST_SIZE));
    puts("PASS: guest pages reclaim/coalesce, remain zeroed, preserve explicit pools, reject non-owner frees, and survive 4 GiB turnover plus concurrent churn.");
}
