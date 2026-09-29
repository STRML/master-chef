/* Production ReadFileEx and alertable dispatch against a disposable file.
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_win32_apc_queue.c -Wl,-dead_strip -o /tmp/test_win32_apc_queue
 */
#include <unistd.h>
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <sched.h>
static _Atomic unsigned read_calls;
static ssize_t observed_pread(int fd, void *buf, size_t n, off_t offset) {
    ++read_calls;
    return pread(fd, buf, n, offset);
}
#define pread observed_pread
#include "../shims_kernel32.c"
#undef pread
uint8_t *engine_flat_base;
static FileObj file;
static _Thread_local uint32_t error_code;
static _Thread_local uint32_t seen[256], seen_bytes[256];
static _Thread_local unsigned callback_count;
static _Thread_local int reenter;
static _Atomic int worker_queued, worker_dispatch;
static jmp_buf process_exit;
static int process_exit_code;
void host_set_last_error(uint32_t e) { error_code = e; }
void *host_handle_object(uint32_t h, int kind) { return h == 64 && kind == HANDLE_FILE ? &file : NULL; }
void host_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void host_exit(int code) { process_exit_code = code; longjmp(process_exit, 1); }
uint32_t host_wait_single(uint32_t h, uint32_t timeout) { (void)h; (void)timeout; return HOST_WAIT_TIMEOUT; }
static uint32_t call(EngineCPU *cpu, HostShim fn, unsigned argc, const uint32_t *argv) {
    uint32_t stack = cpu->fs_base ? cpu->fs_base : 0x1000;
    cpu->gpr[4] = stack;
    S32(stack, 0x12345678);
    for (unsigned i = 0; i < argc; i++) S32(stack + 4 + 4*i, argv[i]);
    fn(cpu);
    assert(cpu->pc == 0x12345678 && cpu->gpr[4] == stack + 4 + 4*argc);
    return cpu->gpr[0];
}
static uint32_t read_ex(EngineCPU *cpu, unsigned id, uint32_t offset, uint32_t size) {
    uint32_t ov = 0x3000 + id * 24;
    S32(ov + 8, offset); S32(ov + 12, 0);
    uint32_t args[] = {64, 0x6000 + id * 16, size, ov, 0x123000};
    return call(cpu, shim_ReadFileEx, 5, args);
}
static uint32_t alertable(EngineCPU *cpu) {
    uint32_t args[] = {88, 0, 1};
    return call(cpu, shim_WaitForSingleObjectEx, 3, args);
}
uint32_t host_call_guest(EngineCPU *cpu, uint32_t routine, int argc, const uint32_t *args, int pops) {
    assert(routine == 0x123000 && argc == 3 && pops == 1 && args[0] == 0);
    unsigned id = (args[2] - 0x3000) / 24;
    assert(callback_count < 256);
    seen[callback_count] = id; seen_bytes[callback_count++] = args[1];
    if (reenter && id == 0) {
        reenter = 0;
        /* Existing completions 1 and 2 stay ahead of callback-submitted 3. */
        EngineCPU nested = *cpu;
        assert(read_ex(&nested, 3, 0, 4));
        assert(alertable(&nested) == 0xC0);
    }
    return 0xDEADBEEF; /* Completion callbacks are void; their EAX is ignored. */
}
static void *queue_worker(void *unused) {
    (void)unused;
    EngineCPU cpu = {0}; cpu.fs_base = 0x1800;
    assert(read_ex(&cpu, 70, 0, 4));
    atomic_store(&worker_queued, 1);
    while (!atomic_load(&worker_dispatch)) sched_yield();
    assert(callback_count == 0 && apc_count == 1);
    assert(alertable(&cpu) == 0xC0 && callback_count == 1 && seen[0] == 70);
    return NULL;
}
int main(void) {
    alarm(30);
    engine_flat_base = calloc(1, 0x10000); assert(engine_flat_base);
    char path[] = "/private/tmp/halo-apc-XXXXXX";
    file.fd = mkstemp(path); assert(file.fd >= 0);
    assert(write(file.fd, "0123456789abcdef", 16) == 16);
    EngineCPU cpu = {0};
    /* Reads are counted by the file they come from (the report's cache-file
     * reads); this fixture file is a level map. */
    assert(file_read_kind("maps\\sounds.map") == HOST_READ_SOUNDS_MAP && file_read_kind("MAPS\\BITMAPS.MAP") == HOST_READ_BITMAPS_MAP);
    assert(file_read_kind("maps\\b30.map") == HOST_READ_LEVEL_MAP && file_read_kind("maps/ui.map") == HOST_READ_LEVEL_MAP);
    assert(file_read_kind("savegame.bin") == HOST_READ_OTHER && file_read_kind(".map") == HOST_READ_OTHER);
    for (unsigned i = 0; i < 64; i++) assert(read_ex(&cpu, i, i % 16, 4));
    assert(apc_count == 64 && read_calls == 64 && !callback_count);
    assert(host_async_reads[HOST_READ_LEVEL_MAP] == 64 && host_async_read_bytes[HOST_READ_LEVEL_MAP] == 232);
    memset(GPTR(0x6000 + 64*16), 0xAA, 16);
    S32(0x3000 + 64*24, 0xABCDEF01); S32(0x3004 + 64*24, 0x12345678);
    assert(!read_ex(&cpu, 64, 0, 4) && error_code == 8);
    assert(apc_count == 64 && read_calls == 64 && !callback_count);
    assert(G32(0x6000 + 64*16) == 0xAAAAAAAA);
    assert(G32(0x3000 + 64*24) == 0xABCDEF01 && G32(0x3004 + 64*24) == 0x12345678);
    assert(alertable(&cpu) == 0xC0 && callback_count == 64 && !apc_count);
    for (unsigned i = 0; i < 64; i++) {
        assert(seen[i] == i);
        assert(seen_bytes[i] == (i % 16 > 12 ? 16 - i%16 : 4));
    }
    assert(alertable(&cpu) == HOST_WAIT_TIMEOUT);
    assert(host_async_reads[HOST_READ_LEVEL_MAP] == 64);   /* the refused 65th is not counted */
    file.kind = HOST_READ_SOUNDS_MAP;
    assert(read_ex(&cpu, 0, 0, 4) && alertable(&cpu) == 0xC0);
    assert(host_async_reads[HOST_READ_SOUNDS_MAP] == 1 && host_async_read_bytes[HOST_READ_SOUNDS_MAP] == 4);
    file.kind = HOST_READ_LEVEL_MAP;
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, queue_worker, NULL));
    while (!atomic_load(&worker_queued)) sched_yield();
    callback_count = 0;
    assert(read_ex(&cpu, 71, 0, 4));
    assert(alertable(&cpu) == 0xC0 && callback_count == 1 && seen[0] == 71);
    atomic_store(&worker_dispatch, 1);
    assert(!pthread_join(worker, NULL));
    for (unsigned cycle = 0; cycle < 200; cycle++) {
        callback_count = 0; reenter = 1;
        for (unsigned i = 0; i < 3; i++) assert(read_ex(&cpu, i, 0, 4));
        assert(alertable(&cpu) == 0xC0 && callback_count == 4 && !apc_count);
        for (unsigned i = 0; i < 4; i++) assert(seen[i] == i && seen_bytes[i] == 4);
    }
    callback_count = 0;
    assert(!read_ex(&cpu, 0, 16, 4) && error_code == 38 && !apc_count);
    assert(read_ex(&cpu, 0, 16, 0) && error_code == 0);
    assert(alertable(&cpu) == 0xC0 && seen_bytes[0] == 0);
    uint32_t invalid[] = {64, 0x6000, 4, 0, 0x123000};
    assert(!call(&cpu, shim_ReadFileEx, 5, invalid) && error_code == 87);
    invalid[3] = 0x3000; invalid[4] = 0;
    assert(!call(&cpu, shim_ReadFileEx, 5, invalid) && error_code == 87);
    invalid[0] = 65; invalid[4] = 0x123000;
    assert(!call(&cpu, shim_ReadFileEx, 5, invalid) && error_code == 6);
    close(file.fd); file.fd = -1;
    assert(!read_ex(&cpu, 0, 0, 4) && error_code == 5 && !apc_count);
    assert(!unlink(path));
    uint32_t terminate[] = {123, 69};
    assert(!call(&cpu, shim_TerminateProcess, 2, terminate) && error_code == 6);
    terminate[0] = UINT32_MAX;
    if (!setjmp(process_exit)) { call(&cpu, shim_TerminateProcess, 2, terminate); assert(0); }
    assert(process_exit_code == 69);
    free(engine_flat_base);
    puts("PASS: 64 accepted completions, saturation rejected before I/O/guest writes, FIFO nested dispatch, queue turnover, partial/zero/EOF/error reads, reads counted by file and process-handle validation.");
}
