/* Production truncate/flush shims against disposable files and failed descriptors.
 * clang -O2 -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_win32_save_flush.c -Wl,-dead_strip -o /tmp/test_win32_save_flush */
#include "../shims_kernel32.c"
#include <assert.h>

uint8_t *engine_flat_base;
static FileObj file;
static uint32_t error_code;
void host_set_last_error(uint32_t e) { error_code = e; }
void *host_handle_object(uint32_t h, int kind) {
    assert(kind == HANDLE_FILE);
    return h == 64 ? &file : NULL;
}
static uint32_t call(HostShim fn, uint32_t handle) {
    EngineCPU cpu = {0};
    cpu.gpr[4] = 0x1000;
    S32(0x1000, 0x12345678);
    S32(0x1004, handle);
    error_code = 0;
    fn(&cpu);
    assert(cpu.pc == 0x12345678 && cpu.gpr[4] == 0x1008);
    return cpu.gpr[0];
}
int main(void) {
    engine_flat_base = calloc(1, 0x2000);
    assert(engine_flat_base);
    char path[] = "/private/tmp/halo-save-flush-XXXXXX";
    file.fd = mkstemp(path);
    assert(file.fd >= 0);
    assert(write(file.fd, "checkpoint-tail", 15) == 15);
    assert(lseek(file.fd, 10, SEEK_SET) == 10);
    assert(call(shim_SetEndOfFile, 64));
    struct stat st;
    assert(!fstat(file.fd, &st) && st.st_size == 10);
    assert(call(shim_FlushFileBuffers, 64));
    assert(!close(file.fd));
    file.fd = open(path, O_RDONLY);
    assert(file.fd >= 0);
    char saved[11] = {0};
    assert(read(file.fd, saved, 11) == 10 && !strcmp(saved, "checkpoint"));
    assert(lseek(file.fd, 0, SEEK_SET) == 0);
    assert(!call(shim_SetEndOfFile, 64) && error_code == 5);
    assert(!fstat(file.fd, &st) && st.st_size == 10);
    assert(!close(file.fd));
    /* A table entry with a failing native descriptor must not claim durability. */
    file.fd = -1;
    assert(!call(shim_FlushFileBuffers, 64) && error_code == 5);
    assert(!call(shim_SetEndOfFile, 64) && error_code == 131);
    assert(!call(shim_FlushFileBuffers, 65) && error_code == 6);
    assert(!call(shim_SetEndOfFile, 65) && error_code == 6);
    assert(!unlink(path));
    free(engine_flat_base);
    puts("PASS: save flush uses fsync; truncate changes length; persisted bytes reopen correctly; invalid/read-only/failing descriptors report failure.");
}
