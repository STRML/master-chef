/* Actual Win32 heap shims and production allocator; no fake guest heap.
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_win32_heap_reuse.c -Wl,-dead_strip -o /tmp/test_win32_heap_reuse */
#include "../host.c"
#include "../shims_kernel32.c"
#include <assert.h>
_Thread_local EngineCPU *host_active_cpu;
static uint32_t call(HostShim fn, unsigned count, const uint32_t args[]) {
    EngineCPU cpu = {0}; cpu.gpr[4] = 0x10000;
    S32(cpu.gpr[4], 0x12345678);
    for (unsigned i = 0; i < count; i++) S32(cpu.gpr[4] + 4 + i * 4, args[i]);
    fn(&cpu);
    assert(cpu.pc == 0x12345678 && cpu.gpr[4] == 0x10004 + count * 4);
    return cpu.gpr[0];
}
#define CALL(fn, ...) call(shim_##fn, sizeof((uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t), (uint32_t[]){__VA_ARGS__})
int main(void) {
    engine_flat_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(engine_flat_base != MAP_FAILED);
    uint32_t a = CALL(HeapAlloc, 64, 8, 31);
    memset(GPTR(a), 0xa7, 31);
    S32(a - 4, UINT32_MAX); /* size/copy must trust native metadata */
    assert(CALL(HeapSize, 64, 0, a) == 31);
    uint32_t b = CALL(HeapReAlloc, 64, 8, a, 97);
    for (unsigned i = 0; i < 31; i++) assert(G8(b + i) == 0xa7);
    for (unsigned i = 31; i < 112; i++) assert(G8(b + i) == 0);
    assert(!guest_alloc_size(a) && guest_alloc_size(b) == 97);
    uint32_t c = CALL(HeapReAlloc, 64, 0, b, 7);
    for (unsigned i = 0; i < 7; i++) assert(G8(c + i) == 0xa7);
    assert(!guest_alloc_size(b) && guest_alloc_size(c) == 7);
    assert(CALL(HeapFree, 64, 0, c));
    assert(CALL(HeapFree, 64, 0, c)); /* legacy shim result, allocator remains intact */
    assert(heap_live_blocks == 0);
    a = CALL(GlobalAlloc, 0x40, 41);
    assert(CALL(GlobalLock, a) == a); /* alias, not a second allocation */
    memset(GPTR(a), 0x72, 41);
    b = CALL(GlobalReAlloc, a, 81, 0x40);
    for (unsigned i = 0; i < 41; i++) assert(G8(b + i) == 0x72);
    for (unsigned i = 41; i < 81; i++) assert(G8(b + i) == 0);
    assert(!guest_alloc_size(a));
    assert(CALL(GlobalFree, b) == 0);
    a = CALL(LocalAlloc, 0x40, 513); assert(a == HEAP_START + 16);
    assert(CALL(LocalFree, a) == 0 && heap_live_blocks == 0);
    /* Production realloc allocates before copying/freeing; >2 GiB traffic. */
    a = CALL(HeapAlloc, 64, 8, 262144); S32(a, 0xfedcba98);
    for (unsigned i = 0; i < 8192; i++) {
        b = CALL(HeapReAlloc, 64, 8, a, 262144 + (i & 255));
        assert(G32(b) == 0xfedcba98 && !guest_alloc_size(a)); a = b;
    }
    assert(CALL(HeapFree, 64, 0, a));
    assert(heap_live_blocks == 0 && heap_initial.span == HEAP_END - HEAP_START && heap_slab_count == 1);
    assert(!munmap(engine_flat_base, GUEST_SIZE));
    puts("PASS: production Heap/Global/Local reclaim; realloc grow/shrink copies, zeros growth, validates native sizes, and survives >2 GiB turnover.");
}
