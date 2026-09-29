/* Production TLS slot allocation/reuse with separate native guest threads.
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_win32_tls_lifetime.c -Wl,-dead_strip -o /tmp/test_win32_tls_lifetime
 */
#include "../shims_kernel32.c"
#include <assert.h>
#include <stdatomic.h>
#include <sched.h>
uint8_t *engine_flat_base;
static _Thread_local uint32_t error_code;
static _Atomic unsigned ready, phase;
static _Atomic uint64_t owned_slots;
static uint32_t recycled_index;
void host_set_last_error(uint32_t e) { error_code = e; }
static uint32_t call(EngineCPU *cpu, HostShim fn, unsigned argc, uint32_t a, uint32_t b) {
    uint32_t stack = cpu->fs_base - 0x1000;
    cpu->gpr[4] = stack;
    S32(stack, 0x12345678); S32(stack + 4, a); S32(stack + 8, b);
    fn(cpu);
    assert(cpu->pc == 0x12345678 && cpu->gpr[4] == stack + 4 + 4*argc);
    return cpu->gpr[0];
}
static void *lifetime_worker(void *opaque) {
    uint32_t id = (uint32_t)(uintptr_t)opaque;
    EngineCPU cpu = {0}; cpu.fs_base = 0x2000 + id * 0x4000;
    assert(!call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) && error_code == 0);
    assert(call(&cpu, shim_TlsSetValue, 2, recycled_index, 0xA000 + id));
    assert(call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) == 0xA000 + id);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&phase)) sched_yield();
    assert(!call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) && error_code == 0);
    assert(call(&cpu, shim_TlsSetValue, 2, recycled_index, 0xB000 + id));
    assert(call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) == 0xB000 + id);
    return NULL;
}
static void *turnover_worker(void *opaque) {
    uint32_t id = (uint32_t)(uintptr_t)opaque;
    EngineCPU cpu = {0}; cpu.fs_base = 0x2000 + id * 0x4000;
    for (unsigned n = 0; n < 4000; n++) {
        uint32_t index = call(&cpu, shim_TlsAlloc, 0, 0, 0);
        assert(index > 0 && index < 64);
        uint64_t bit = UINT64_C(1) << index;
        assert(!(atomic_fetch_or(&owned_slots, bit) & bit));
        assert(!call(&cpu, shim_TlsGetValue, 1, index, 0) && error_code == 0);
        uint32_t value = (id << 24) | (n + 1);
        assert(call(&cpu, shim_TlsSetValue, 2, index, value));
        assert(call(&cpu, shim_TlsGetValue, 1, index, 0) == value);
        assert(atomic_fetch_and(&owned_slots, ~bit) & bit);
        assert(call(&cpu, shim_TlsFree, 1, index, 0));
    }
    return NULL;
}
int main(void) {
    alarm(30);
    engine_flat_base = calloc(1, 0x20000); assert(engine_flat_base);
    EngineCPU cpu = {0}; cpu.fs_base = 0x2000;
    uint32_t slots[63];
    for (unsigned i = 0; i < 63; i++) {
        slots[i] = call(&cpu, shim_TlsAlloc, 0, 0, 0);
        assert(slots[i] == i + 1);
        assert(call(&cpu, shim_TlsSetValue, 2, slots[i], 0xCAFE0000 + i));
    }
    assert(call(&cpu, shim_TlsAlloc, 0, 0, 0) == UINT32_MAX);
    recycled_index = slots[0];
    pthread_t workers[4];
    for (uintptr_t i = 1; i <= 3; i++) assert(!pthread_create(&workers[i-1], NULL, lifetime_worker, (void *)i));
    while (atomic_load(&ready) != 3) sched_yield();
    assert(call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) == 0xCAFE0000);
    assert(call(&cpu, shim_TlsFree, 1, recycled_index, 0));
    assert(call(&cpu, shim_TlsAlloc, 0, 0, 0) == recycled_index);
    assert(call(&cpu, shim_TlsAlloc, 0, 0, 0) == UINT32_MAX && error_code == 8);
    assert(!call(&cpu, shim_TlsGetValue, 1, recycled_index, 0) && error_code == 0);
    atomic_store(&phase, 1);
    for (unsigned i = 0; i < 3; i++) assert(!pthread_join(workers[i], NULL));
    for (unsigned i = 1; i < 63; i++) assert(call(&cpu, shim_TlsGetValue, 1, slots[i], 0) == 0xCAFE0000 + i);
    for (unsigned i = 0; i < 63; i++) assert(call(&cpu, shim_TlsFree, 1, slots[i], 0));
    assert(!call(&cpu, shim_TlsFree, 1, recycled_index, 0) && error_code == 87);
    assert(!call(&cpu, shim_TlsFree, 1, 0, 0) && error_code == 87);
    assert(!call(&cpu, shim_TlsFree, 1, 64, 0) && error_code == 87);
    assert(!call(&cpu, shim_TlsGetValue, 1, 64, 0) && error_code == 87);
    assert(!call(&cpu, shim_TlsSetValue, 2, 64, 42) && error_code == 87);
    for (uintptr_t i = 1; i <= 4; i++) assert(!pthread_create(&workers[i-1], NULL, turnover_worker, (void *)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(workers[i], NULL));
    assert(!atomic_load(&owned_slots) && tls_allocated == 1);
    free(engine_flat_base);
    puts("PASS: full TLS exhaustion/reuse, no duplicate live slots, cross-thread recycled values zeroed, thread isolation, invalid/free validation and 16000 concurrent allocation lifetimes.");
}
