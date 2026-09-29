/* KERNEL32 stand-ins: process, memory, files, time, locale, and pthread-backed sync. */
#include <pthread.h>
#include "host.h"
#include "threading.h"
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <strings.h>

extern const char *host_command_line;
static int deliver_apcs(EngineCPU *cpu);
static uint64_t start_ns;
static uint32_t env_block_a, env_block_w, cmdline_guest, modname_guest;
/* Preserve the host's reserved slot zero. Generations initialize recycled
 * slots on each owning thread without writing another running thread's TEB.
 * One guest TEB belongs to each native engine thread for its entire lifetime. */
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_allocated = 1;
static uint64_t tls_generation[64];
static _Thread_local uint64_t tls_seen_generation[64];
static uint32_t std_handles[3] = { 0x100, 0x104, 0x108 };

static uint64_t ms_since_start(void) { if (!start_ns) start_ns = host_monotonic_ns(); return (host_monotonic_ns() - start_ns) / 1000000u; }
static uint64_t unix_to_filetime(time_t t, long ns) { return (uint64_t)t * 10000000u + (uint64_t)ns / 100u + UINT64_C(116444736000000000); }
static void write_systemtime(uint32_t a, struct tm *tm, uint32_t ms) {
    S16(a, (uint16_t)(tm->tm_year + 1900)); S16(a + 2, (uint16_t)(tm->tm_mon + 1)); S16(a + 4, (uint16_t)tm->tm_wday); S16(a + 6, (uint16_t)tm->tm_mday);
    S16(a + 8, (uint16_t)tm->tm_hour); S16(a + 10, (uint16_t)tm->tm_min); S16(a + 12, (uint16_t)tm->tm_sec); S16(a + 14, (uint16_t)ms);
}
static uint32_t copy_out(uint32_t dst, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(s); if (!dst || !cap) return n + 1;
    if (n >= cap) n = cap - 1; memcpy(GPTR(dst), s, n); S8(dst + n, 0); return n;
}

/* ---- process / module ---- */
SHIM(GetModuleHandleA) { uint32_t name = ARG(0); uint32_t h = engine_pe_image_base;
    if (name) { const char *s = GSTR(name); if (!strcasestr(s, "halo")) { h = host_module_handle(s); if (!h) host_set_last_error(126); } }
    RET_STDCALL(h, 1); }
SHIM(GetModuleFileNameA) { uint32_t n = copy_out(ARG(1), ARG(2), "C:\\Halo\\halo.exe"); RET_STDCALL(n, 3); }
SHIM(LoadLibraryA) { const char *s = GSTR(ARG(0)); uint32_t h = host_module_handle(s);
    host_trace("[dll] LoadLibraryA(%s) -> %08X", s, h); if (!h) host_set_last_error(126); RET_STDCALL(h, 1); }
SHIM(FreeLibrary) { RET_STDCALL(1, 1); }
SHIM(GetProcAddress) { uint32_t hmod = ARG(0), nm = ARG(1); const char *dll = host_module_name(hmod);
    if (hmod == engine_pe_image_base) dll = "halo.exe";
    char namebuf[32]; const char *name = (nm & 0xFFFF0000u) ? GSTR(nm) : (snprintf(namebuf, sizeof namebuf, "#%u", nm & 0xFFFFu), namebuf);
    uint32_t r = 0;
    if (dll) {
        r = host_proc_address(dll, name);
        /* GetProcAddress exposes only exports this host can actually call.  Static
           imports still receive a magic address during PE binding and fail loudly
           if called without a shim; optional dynamic imports receive Windows'
           normal ERROR_PROC_NOT_FOUND result so the game can take its guard. */
        if (!host_proc_has_shim(r)) r = 0;
    }
    host_trace("[dll] GetProcAddress(%s, %s) -> %08X", dll ? dll : "?", name, r);
    if (!r) host_set_last_error(127);
    RET_STDCALL(r, 2); }
SHIM(GetCommandLineA) { if (!cmdline_guest) cmdline_guest = guest_strdup(host_command_line); RET_STDCALL(cmdline_guest, 0); }
SHIM(GetStartupInfoA) { uint32_t si = ARG(0); memset(GPTR(si), 0, 68); S32(si, 68); S16(si + 0x30, 1); S32(si + 0x38, 0x100); S32(si + 0x3C, 0x104); S32(si + 0x40, 0x108); RET_STDCALL(0, 1); }
SHIM(GetVersionExA) { uint32_t v = ARG(0); uint32_t size = G32(v); memset(GPTR(v + 4), 0, size - 4);
    S32(v + 4, 5); S32(v + 8, 1); S32(v + 12, 2600); S32(v + 16, 2); strcpy((char *)GPTR(v + 20), "Service Pack 3");
    if (size >= 156) { S16(v + 148, 3); S16(v + 150, 0); S16(v + 152, 0x100); S8(v + 154, 1); }
    RET_STDCALL(1, 1); }
SHIM(GetSystemInfo) { uint32_t si = ARG(0); memset(GPTR(si), 0, 36); S32(si + 4, 4096); S32(si + 8, 0x10000); S32(si + 12, 0x7FFEFFFF); S32(si + 16, 1); S32(si + 20, 1); S32(si + 24, 586); S32(si + 28, 65536); S16(si + 32, 6); S16(si + 34, 0x0803); RET_STDCALL(0, 1); }
SHIM(GetEnvironmentStrings) { if (!env_block_a) { env_block_a = guest_alloc(64); memcpy(GPTR(env_block_a), "SystemRoot=C:\\WINDOWS\0\0", 24); } RET_STDCALL(env_block_a, 0); }
SHIM(GetEnvironmentStringsW) { if (!env_block_w) { env_block_w = guest_alloc(128); const char *s = "SystemRoot=C:\\WINDOWS"; for (size_t i = 0; s[i]; i++) S16(env_block_w + 2 * (uint32_t)i, (uint16_t)s[i]); } RET_STDCALL(env_block_w, 0); }
SHIM(FreeEnvironmentStringsA) { RET_STDCALL(1, 1); }
SHIM(FreeEnvironmentStringsW) { RET_STDCALL(1, 1); }
SHIM(SetEnvironmentVariableA) { RET_STDCALL(1, 2); }
SHIM(GetStdHandle) { uint32_t n = ARG(0); uint32_t h = n == 0xFFFFFFF6u ? 0x100 : n == 0xFFFFFFF5u ? 0x104 : n == 0xFFFFFFF4u ? 0x108 : 0xFFFFFFFFu; RET_STDCALL(h, 1); }
SHIM(SetStdHandle) { RET_STDCALL(1, 2); }
SHIM(GetFileType) { uint32_t h = ARG(0); uint32_t t = (h == 0x100 || h == 0x104 || h == 0x108) ? 2 : host_handle_object(h, HANDLE_FILE) ? 1 : 0; RET_STDCALL(t, 1); }
SHIM(SetHandleCount) { RET_STDCALL(ARG(0), 1); }
SHIM(GetCurrentProcess) { RET_STDCALL(0xFFFFFFFFu, 0); }
SHIM(GetCurrentThread) { RET_STDCALL(0xFFFFFFFEu, 0); }
SHIM(GetCurrentProcessId) { RET_STDCALL(4660, 0); }
SHIM(GetCurrentThreadId) { RET_STDCALL(host_current_thread_id(), 0); }
SHIM(ExitProcess) { host_log("ExitProcess(%u)", ARG(0)); host_exit((int)ARG(0)); }
SHIM(TerminateProcess) {
    /* No real process handles are issued by this host. Only the current
     * process pseudo-handle is valid; an invalid target must not stop Halo. */
    if (ARG(0) != 0xFFFFFFFFu) { host_set_last_error(6); RET_STDCALL(0, 2); }
    host_log("TerminateProcess(current, %u)", ARG(1)); host_exit((int)ARG(1)); }
SHIM(SetUnhandledExceptionFilter) { RET_STDCALL(0, 1); }
SHIM(UnhandledExceptionFilter) { RET_STDCALL(1, 1); }
SHIM(SetErrorMode) { RET_STDCALL(0, 1); }
SHIM(GetLastError) { RET_STDCALL(host_last_error, 0); }
SHIM(SetLastError) { host_set_last_error(ARG(0)); RET_STDCALL(0, 1); }
SHIM(IsProcessorFeaturePresent) { RET_STDCALL(0, 1); }
SHIM(IsBadReadPtr) { RET_STDCALL(0, 2); }
SHIM(IsBadWritePtr) { RET_STDCALL(0, 2); }
SHIM(IsBadCodePtr) { RET_STDCALL(0, 1); }
SHIM(RaiseException) { host_log("RaiseException(code %08X) from %08X", ARG(0), G32(cpu->gpr[4])); engine_fail(cpu, "RaiseException"); }
SHIM(RtlUnwind) { engine_fail(cpu, "RtlUnwind (SEH unwinding is not modeled)"); }
SHIM(FormatMessageA) { uint32_t n = copy_out(ARG(4), ARG(5), "Unknown error"); RET_STDCALL(n, 7); }

/* ---- memory ---- */
SHIM(HeapCreate) { RET_STDCALL(host_handle_new(HANDLE_HEAP, NULL), 3); }
SHIM(HeapDestroy) { host_handle_close(ARG(0)); RET_STDCALL(1, 1); }
SHIM(GetProcessHeap) { static uint32_t h; if (!h) h = host_handle_new(HANDLE_HEAP, NULL); RET_STDCALL(h, 0); }
SHIM(HeapAlloc) { RET_STDCALL(guest_alloc(ARG(2)), 3); }
SHIM(HeapFree) { guest_free(ARG(2)); RET_STDCALL(1, 3); }
SHIM(HeapSize) { RET_STDCALL(guest_alloc_size(ARG(2)), 3); }
SHIM(HeapReAlloc) { uint32_t old = ARG(2), size = ARG(3); uint32_t n = guest_alloc(size);
    if (old) { uint32_t o = guest_alloc_size(old); memcpy(GPTR(n), GPTR(old), o < size ? o : size); guest_free(old); } RET_STDCALL(n, 4); }
SHIM(GlobalAlloc) { RET_STDCALL(guest_alloc(ARG(1)), 2); }
SHIM(GlobalFree) { guest_free(ARG(0)); RET_STDCALL(0, 1); }
SHIM(GlobalLock) { RET_STDCALL(ARG(0), 1); }
SHIM(GlobalUnlock) { RET_STDCALL(1, 1); }
SHIM(GlobalReAlloc) { uint32_t old = ARG(0), size = ARG(1); uint32_t n = guest_alloc(size);
    if (old) { uint32_t o = guest_alloc_size(old); memcpy(GPTR(n), GPTR(old), o < size ? o : size); guest_free(old); } RET_STDCALL(n, 3); }
SHIM(LocalAlloc) { RET_STDCALL(guest_alloc(ARG(1)), 2); }
SHIM(LocalFree) { guest_free(ARG(0)); RET_STDCALL(0, 1); }
SHIM(GlobalMemoryStatus) { uint32_t m = ARG(0); S32(m, 32); S32(m + 4, 30); S32(m + 8, 0x7FFF0000u); S32(m + 12, 0x60000000u); S32(m + 16, 0x7FFF0000u); S32(m + 20, 0x60000000u); S32(m + 24, 0x7FFE0000u); S32(m + 28, 0x70000000u); RET_STDCALL(0, 1); }
uint32_t guest_page_reserve_explicit(uint32_t addr, uint32_t size);
SHIM(VirtualAlloc) { uint32_t addr = ARG(0), size = ARG(1); uint32_t r;
    if (addr >= 0x40000000u && addr < 0x7F000000u) r = guest_page_reserve_explicit(addr, size); else r = guest_page_alloc(size);
    if (!r) host_set_last_error(size ? 487u : 87u);
    host_trace("[mem] VirtualAlloc(%08X, %u, type %X) -> %08X", addr, size, ARG(2), r); RET_STDCALL(r, 4); }
SHIM(VirtualFree) { RET_STDCALL(1, 3); }
SHIM(VirtualProtect) { if (ARG(3)) S32(ARG(3), 4); RET_STDCALL(1, 4); }
SHIM(VirtualQuery) { uint32_t a = ARG(0), mbi = ARG(1); uint32_t base = a & ~0xFFFu;
    S32(mbi, base); S32(mbi + 4, base); S32(mbi + 8, 4); S32(mbi + 12, 0x1000); S32(mbi + 16, 0x1000); S32(mbi + 20, 4); S32(mbi + 24, 0x20000); RET_STDCALL(28, 3); }
SHIM(TlsAlloc) {
    uint32_t index = 0xFFFFFFFFu;
    pthread_mutex_lock(&tls_lock);
    for (uint32_t i = 1; i < 64; i++) {
        if (!(tls_allocated & (UINT64_C(1) << i)) && tls_generation[i] != UINT64_MAX) {
            tls_allocated |= UINT64_C(1) << i;
            ++tls_generation[i];
            index = i;
            break;
        }
    }
    pthread_mutex_unlock(&tls_lock);
    if (index == 0xFFFFFFFFu) host_set_last_error(8);
    RET_STDCALL(index, 0); }
SHIM(TlsFree) {
    uint32_t index = ARG(0);
    int valid = 0;
    pthread_mutex_lock(&tls_lock);
    if (index > 0 && index < 64 && (tls_allocated & (UINT64_C(1) << index))) {
        tls_allocated &= ~(UINT64_C(1) << index);
        valid = 1;
    }
    pthread_mutex_unlock(&tls_lock);
    if (!valid) host_set_last_error(87);
    RET_STDCALL(valid, 1); }
static uint32_t tls_slot(EngineCPU *cpu, uint32_t index) {
    uint32_t slot = (cpu->fs_base ? cpu->fs_base : ENGINE_DEFAULT_FS_BASE) + 0xE10u + 4u * index;
    if (tls_seen_generation[index] != tls_generation[index]) {
        S32(slot, 0);
        tls_seen_generation[index] = tls_generation[index];
    }
    return slot;
}
SHIM(TlsGetValue) {
    uint32_t index = ARG(0);
    if (index >= 64u) { host_set_last_error(87); RET_STDCALL(0, 1); }
    pthread_mutex_lock(&tls_lock);
    uint32_t value = G32(tls_slot(cpu, index));
    pthread_mutex_unlock(&tls_lock);
    host_set_last_error(0);
    RET_STDCALL(value, 1); }
SHIM(TlsSetValue) {
    uint32_t index = ARG(0), value = ARG(1);
    if (index >= 64u) { host_set_last_error(87); RET_STDCALL(0, 2); }
    pthread_mutex_lock(&tls_lock);
    S32(tls_slot(cpu, index), value);
    pthread_mutex_unlock(&tls_lock);
    RET_STDCALL(1, 2); }
SHIM(InitializeCriticalSection) { host_critical_section_initialize(ARG(0)); RET_STDCALL(0, 1); }
SHIM(DeleteCriticalSection) { host_critical_section_delete(ARG(0)); RET_STDCALL(0, 1); }
SHIM(EnterCriticalSection) { host_critical_section_enter(ARG(0)); RET_STDCALL(0, 1); }
SHIM(LeaveCriticalSection) { host_critical_section_leave(ARG(0)); RET_STDCALL(0, 1); }
SHIM(InterlockedExchange) { uint32_t p = ARG(0), v = ARG(1); uint32_t old = __atomic_exchange_n((uint32_t *)GPTR(p), v, __ATOMIC_SEQ_CST); RET_STDCALL(old, 2); }

/* ---- time ---- */
SHIM(GetTickCount) { RET_STDCALL((uint32_t)ms_since_start() + 100000u, 0); }
SHIM(QueryPerformanceCounter) { S64(ARG(0), host_monotonic_ns()); RET_STDCALL(1, 1); }
SHIM(QueryPerformanceFrequency) { S64(ARG(0), UINT64_C(1000000000)); RET_STDCALL(1, 1); }
SHIM(GetSystemTimeAsFileTime) { S64(ARG(0), host_filetime_now()); RET_STDCALL(0, 1); }
SHIM(GetSystemTime) { struct timeval tv; gettimeofday(&tv, NULL); struct tm tm; gmtime_r(&tv.tv_sec, &tm); write_systemtime(ARG(0), &tm, (uint32_t)(tv.tv_usec / 1000)); RET_STDCALL(0, 1); }
SHIM(GetLocalTime) { struct timeval tv; gettimeofday(&tv, NULL); struct tm tm; localtime_r(&tv.tv_sec, &tm); write_systemtime(ARG(0), &tm, (uint32_t)(tv.tv_usec / 1000)); RET_STDCALL(0, 1); }
SHIM(GetTimeZoneInformation) { memset(GPTR(ARG(0)), 0, 172); RET_STDCALL(0, 1); }
SHIM(SystemTimeToFileTime) { uint32_t st = ARG(0); struct tm tm = {0}; tm.tm_year = G16(st) - 1900; tm.tm_mon = G16(st + 2) - 1; tm.tm_mday = G16(st + 6); tm.tm_hour = G16(st + 8); tm.tm_min = G16(st + 10); tm.tm_sec = G16(st + 12);
    S64(ARG(1), unix_to_filetime(timegm(&tm), (long)G16(st + 14) * 1000000L)); RET_STDCALL(1, 2); }
SHIM(CompareFileTime) { uint64_t a = G64(ARG(0)), b = G64(ARG(1)); RET_STDCALL(a < b ? 0xFFFFFFFFu : a > b ? 1 : 0, 2); }
/* How often the game sleeps and for how long, for the device report.
 *
 * Sleep(0) is "give the rest of my timeslice away", and Halo's main loop
 * says it constantly. On an idle Mac sched_yield returns at once. On the
 * headset the same core is wanted by a compositor running at 90 Hz, and
 * every yield hands it over: a fixed cost per frame that no reduction in
 * the engine's own work can touch, which is what the headset showed when
 * drawing half the bearings changed nothing. The engine thread is the only
 * game thread that matters and nothing it waits for needs it to yield, so
 * Sleep(0) no longer does; Sleep(n) still sleeps, and both are counted. */
uint64_t host_sleep_calls, host_sleep_ns, host_yield_calls, host_yield_spin_ns;

/* The engine's waits on the cache reader.
 *
 * Halo's blocking cache reads, 00443E10 and 00444550, wait for the reader
 * thread (00443940) by testing their request's done flag and calling
 * Sleep(0) until it is set; those calls return to 00443EEA and 004446D8. The
 * flag is set only by the read's completion routine (00443AE0), which the
 * reader runs from its own alertable waits, here in deliver_apcs, since
 * 00442C70 retries ReadFileEx until it is accepted. So at those two sites
 * Sleep(0) waits until the reader has delivered a completion, or a
 * millisecond has passed, instead of spinning the engine's core while a cold
 * read comes off flash. The loop is still Halo's: it tests its flag, reads
 * the clock and runs its 132 ms housekeeping (00549960) after every return,
 * exactly as before. A completion delivered since this thread last returned
 * from here sends it straight back to its test, so none is slept through;
 * the wait is bounded, so progress never depends on the signal; and io_lock
 * is only ever held around the counter, never while taking another lock.
 * HALO_IO_WAIT=0 restores the spin. */
uint64_t host_cache_wait_calls, host_cache_wait_ns, host_cache_reads, host_cache_read_ns;
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_completed = PTHREAD_COND_INITIALIZER;
static uint64_t io_completions;                    /* guarded by io_lock */
static _Thread_local uint64_t io_completions_seen;
static _Atomic int cache_wait_mode = -1;           /* -1 until HALO_IO_WAIT is read */
static uint64_t cache_wait_limit_ns = 1000000ull;
int host_cache_wait_blocks(void) {
    if (cache_wait_mode < 0) { const char *option = getenv("HALO_IO_WAIT"); cache_wait_mode = !(option && option[0] == '0'); }
    return cache_wait_mode;
}
static int cache_wait_site(uint32_t return_address) {
    return return_address == 0x00443EEAu || return_address == 0x004446D8u;
}
static void io_signal_completion(void) {
    pthread_mutex_lock(&io_lock);
    io_completions++;
    pthread_cond_broadcast(&io_completed);
    pthread_mutex_unlock(&io_lock);
}
static void io_wait_for_completion(uint64_t timeout_ns) {
    int old_cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
    pthread_mutex_lock(&io_lock);
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + timeout_ns;
    for (;;) {
        if (io_completions != io_completions_seen) break;
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now >= deadline) break;
        uint64_t left = deadline - now;
        struct timespec wait = { (time_t)(left / 1000000000ull), (long)(left % 1000000000ull) };
        pthread_cond_timedwait_relative_np(&io_completed, &io_lock, &wait);
    }
    io_completions_seen = io_completions;
    pthread_mutex_unlock(&io_lock);
    pthread_setcancelstate(old_cancel_state, NULL);
}

/* Presenting-thread elapsed time around existing blocking idle operations
 * (usleep and cache I/O completion waits), including scheduler overshoot.
 * Reuses idle timestamps without another clock read or behavior change;
 * distinct from the limiter's on-core spin. These are elapsed calls, not an
 * exact measure of off-core time. Any new blocking idle on the presenting
 * thread that adds to host_yield_spin_ns must add here too, or the report's
 * unaccounted off-core time takes it (CORE_TELEMETRY.md). */
uint64_t host_present_sleep_ns;
/* The last Sleep(0) on this thread, for timing the limiter's spin. File
 * scope only so the frame pacer's own wait (host_frame_idle_wait) can stop
 * the next Sleep(0) timing the gap across it; Sleep and SleepEx themselves
 * are Build78's, unchanged, including the function-local burst counter. */
static _Thread_local uint64_t last_yield_ns;
/* Called only by the frame pacer (frame_pacing_hooks.inc) around its blocking
 * wait, which happens only while pacing is enabled; with pacing off nothing
 * calls this and the Sleep/SleepEx accounting below is unchanged. The wait is
 * the presenting thread idling, so it counts once towards the frame's idle
 * time, and the next Sleep(0) must not time the gap across it as spin too. */
void host_frame_idle_wait(uint64_t start, uint64_t end) {
    extern pthread_t host_present_thread; extern int host_present_thread_set;
    last_yield_ns = 0;
    if (end > start && host_present_thread_set && pthread_equal(pthread_self(), host_present_thread))
        host_yield_spin_ns += end - start, host_present_sleep_ns += end - start;
}
SHIM(Sleep) {
    uint32_t ms = ARG(0);
    extern pthread_t host_present_thread; extern int host_present_thread_set;
    int presenting = host_present_thread_set && pthread_equal(pthread_self(), host_present_thread);
    if (ms) {
        host_sleep_calls++;
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        usleep(ms * 1000u);
        uint64_t slept = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
        host_sleep_ns += slept;
        /* The presenting thread asleep is the frame idling as much as its
         * spin is. Left out, the bearing budget read 24-30 ms of busy time at
         * Halo's 30 fps cap whatever the engine did, even in the menu at nine
         * one-millisecond passes, and could never step back down to stereo. */
        if (presenting) { host_yield_spin_ns += slept; host_present_sleep_ns += slept; }
    } else {
        /* Sleep(0) is Halo's frame limiter's spin: when the engine is ahead
         * of its cap it calls this in a tight loop, fifty thousand times a
         * frame on the desktop at 30 fps, and with the yield gone that loop
         * pins a core doing nothing. Let the first calls of a burst return
         * at once, so the yield-per-call cost this replaced stays gone, and
         * once a burst is clearly a spin give the core up for a quarter of
         * a millisecond at a time. */
        static _Thread_local unsigned burst;
        host_yield_calls++;
        /* Time spent between yields that follow each other closely is the
         * limiter's spin, which is the engine idling at its cap. The bearing
         * budget subtracts it from the frame to see how much of the frame
         * the engine actually used, and draws more of the sphere with the
         * rest. In play the yields are sparse and the gaps are real work,
         * so a two-millisecond ceiling keeps those out of the count. */
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        /* Only the presenting thread's spin is the frame's idle time; the
         * game's other thread yields on its own account and must not be
         * counted against the engine's frame, or the bearing budget reads
         * spare time that is not there and over-commits. */
        if (presenting && last_yield_ns && now - last_yield_ns < 2000000ull) host_yield_spin_ns += now - last_yield_ns;
        last_yield_ns = now;
        if (cache_wait_site(G32(cpu->gpr[4]))) {
            /* Timed the same way in both modes, so HALO_IO_WAIT can be
             * compared: the gaps between one wait's calls, plus any block. */
            static _Thread_local uint64_t last_cache_wait_ns;
            if (last_cache_wait_ns && now - last_cache_wait_ns < 2000000ull) host_cache_wait_ns += now - last_cache_wait_ns;
            last_cache_wait_ns = now;
            host_cache_wait_calls++;
            if (host_cache_wait_blocks()) {
                io_wait_for_completion(cache_wait_limit_ns);
                uint64_t after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                host_cache_wait_ns += after - now;
                /* Counted as the spin it replaces was, so the bearing budget
                 * reads a frame that waited on a read the way it did before. */
                if (presenting) { host_yield_spin_ns += after - now; host_present_sleep_ns += after - now; }
                last_yield_ns = last_cache_wait_ns = after;
                RET_STDCALL(0, 1);
            }
        }
        /* A frame in play makes 1-68 of these; only the limiter's spin
         * reaches the hundreds, so play never pays the sleep at all. The
         * give-up itself is idle however long the scheduler keeps the core,
         * and the next gap is timed from after it: an overrun past the 2 ms
         * ceiling used to drop the whole gap from the count. */
        if (++burst >= 256) {
            burst = 0; usleep(250);
            uint64_t after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (presenting) { host_yield_spin_ns += after - now; host_present_sleep_ns += after - now; }
            last_yield_ns = after;
        }
    }
    RET_STDCALL(0, 1);
}
SHIM(SleepEx) {
    uint32_t ms = ARG(0);
    if (ARG(1) && deliver_apcs(cpu)) RET_STDCALL(0xC0, 2);
    if (ms && ms != 0xFFFFFFFFu) {
        host_sleep_calls++;
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        usleep(ms * 1000u);
        host_sleep_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    }
    RET_STDCALL(0, 2);
}
SHIM(GetDateFormatA) { uint32_t n = copy_out(ARG(4), ARG(5), "9/13/2026"); RET_STDCALL(n + 1, 6); }
SHIM(GetTimeFormatA) { uint32_t n = copy_out(ARG(4), ARG(5), "12:00:00"); RET_STDCALL(n + 1, 6); }

/* ---- locale / code pages ---- */
SHIM(GetACP) { RET_STDCALL(1252, 0); }
SHIM(GetOEMCP) { RET_STDCALL(437, 0); }
SHIM(GetCPInfo) { uint32_t o = ARG(1); memset(GPTR(o), 0, 20); S32(o, 1); S8(o + 4, '?'); RET_STDCALL(1, 2); }
SHIM(IsValidCodePage) { RET_STDCALL(1, 1); }
SHIM(IsValidLocale) { RET_STDCALL(1, 2); }
SHIM(GetUserDefaultLCID) { RET_STDCALL(0x409, 0); }
SHIM(GetThreadLocale) { RET_STDCALL(0x409, 0); }
SHIM(SetThreadLocale) { RET_STDCALL(1, 1); }
SHIM(GetLocaleInfoA) { uint32_t type = ARG(1) & 0xFFFF; const char *s = type == 0x1004 ? "1252" : type == 0x000B ? "437" : type == 0x1001 ? "English" : type == 0x1002 ? "United States" : type == 0x59 ? "en" : type == 0x5A ? "US" : type == 0x0001 ? "0409" : "";
    uint32_t n = copy_out(ARG(2), ARG(3), s); RET_STDCALL(n + 1, 4); }
SHIM(GetLocaleInfoW) { uint32_t buf = ARG(2), cap = ARG(3); const char *s = "1252"; uint32_t n = (uint32_t)strlen(s);
    if (buf && cap) { if (n >= cap) n = cap - 1; for (uint32_t i = 0; i < n; i++) S16(buf + 2 * i, (uint16_t)s[i]); S16(buf + 2 * n, 0); } RET_STDCALL(n + 1, 4); }
static uint32_t ctype1(unsigned char c) { uint32_t f = 0; if (isupper(c)) f |= 1; if (islower(c)) f |= 2; if (isdigit(c)) f |= 4; if (isspace(c)) f |= 8; if (ispunct(c)) f |= 0x10; if (iscntrl(c)) f |= 0x20; if (c == ' ' || c == '\t') f |= 0x40; if (isxdigit(c)) f |= 0x80; if (isalpha(c)) f |= 0x100; return f; }
SHIM(GetStringTypeA) { uint32_t src = ARG(2), n = ARG(3), out = ARG(4); if (n == 0xFFFFFFFFu) n = (uint32_t)strlen(GSTR(src)) + 1;
    for (uint32_t i = 0; i < n; i++) S16(out + 2 * i, (uint16_t)ctype1(G8(src + i))); RET_STDCALL(1, 5); }
SHIM(GetStringTypeW) { uint32_t src = ARG(1), n = ARG(2), out = ARG(3); if (n == 0xFFFFFFFFu) { n = 0; while (G16(src + 2 * n)) n++; n++; }
    for (uint32_t i = 0; i < n; i++) { uint32_t c = G16(src + 2 * i); S16(out + 2 * i, (uint16_t)(c < 256 ? ctype1((unsigned char)c) : 0x100)); } RET_STDCALL(1, 4); }
SHIM(LCMapStringA) { uint32_t flags = ARG(1), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu) n = (uint32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET_STDCALL(n, 6);
    if (n > cap) { host_set_last_error(122); RET_STDCALL(0, 6); }
    for (uint32_t i = 0; i < n; i++) { unsigned char c = G8(src + i); if (flags & 0x100) c = (unsigned char)tolower(c); else if (flags & 0x200) c = (unsigned char)toupper(c); S8(dst + i, c); }
    RET_STDCALL(n, 6); }
SHIM(LCMapStringW) { uint32_t flags = ARG(1), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu) { n = 0; while (G16(src + 2 * n)) n++; n++; }
    if (!cap) RET_STDCALL(n, 6);
    if (n > cap) { host_set_last_error(122); RET_STDCALL(0, 6); }
    for (uint32_t i = 0; i < n; i++) { uint32_t c = G16(src + 2 * i); if (c < 256) { if (flags & 0x100) c = (uint32_t)tolower((int)c); else if (flags & 0x200) c = (uint32_t)toupper((int)c); } S16(dst + 2 * i, (uint16_t)c); }
    RET_STDCALL(n, 6); }
SHIM(MultiByteToWideChar) { uint32_t src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu) n = (uint32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET_STDCALL(n, 6);
    if (n > cap) { host_set_last_error(122); RET_STDCALL(0, 6); }
    for (uint32_t i = 0; i < n; i++) S16(dst + 2 * i, G8(src + i)); RET_STDCALL(n, 6); }
SHIM(WideCharToMultiByte) { uint32_t src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu) { n = 0; while (G16(src + 2 * n)) n++; n++; }
    if (!cap) RET_STDCALL(n, 8);
    if (n > cap) { host_set_last_error(122); RET_STDCALL(0, 8); }
    for (uint32_t i = 0; i < n; i++) { uint32_t c = G16(src + 2 * i); S8(dst + i, (uint8_t)(c < 256 ? c : '?')); } RET_STDCALL(n, 8); }
SHIM(CompareStringA) { uint32_t a = ARG(2), na = ARG(3), b = ARG(4), nb = ARG(5); int ci = ARG(1) & 1;
    char sa[512], sb[512]; if (na == 0xFFFFFFFFu) snprintf(sa, sizeof sa, "%s", GSTR(a)); else { if (na >= sizeof sa) na = sizeof sa - 1; memcpy(sa, GPTR(a), na); sa[na] = 0; }
    if (nb == 0xFFFFFFFFu) snprintf(sb, sizeof sb, "%s", GSTR(b)); else { if (nb >= sizeof sb) nb = sizeof sb - 1; memcpy(sb, GPTR(b), nb); sb[nb] = 0; }
    int r = ci ? strcasecmp(sa, sb) : strcmp(sa, sb); RET_STDCALL(r < 0 ? 1 : r > 0 ? 3 : 2, 6); }
SHIM(CompareStringW) { uint32_t a = ARG(2), na = ARG(3), b = ARG(4), nb = ARG(5); int ci = ARG(1) & 1; int r = 0;
    for (uint32_t i = 0; ; i++) { uint32_t ca = (na != 0xFFFFFFFFu && i >= na) ? 0 : G16(a + 2 * i), cb = (nb != 0xFFFFFFFFu && i >= nb) ? 0 : G16(b + 2 * i);
        if (ci && ca < 256 && cb < 256) { ca = (uint32_t)tolower((int)ca); cb = (uint32_t)tolower((int)cb); }
        if (ca != cb) { r = ca < cb ? -1 : 1; break; } if (!ca) break; }
    RET_STDCALL(r < 0 ? 1 : r > 0 ? 3 : 2, 6); }
SHIM(EnumSystemLocalesA) { RET_STDCALL(1, 2); }

/* ---- files ---- */
typedef struct { int fd; uint8_t kind; } FileObj;
uint64_t host_async_reads[HOST_READ_KINDS], host_async_read_bytes[HOST_READ_KINDS];
static uint8_t file_read_kind(const char *path) {
    const char *leaf = path; for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/') leaf = p + 1;
    if (!strcasecmp(leaf, "sounds.map")) return HOST_READ_SOUNDS_MAP;
    if (!strcasecmp(leaf, "bitmaps.map")) return HOST_READ_BITMAPS_MAP;
    size_t n = strlen(leaf);
    return n > 4 && !strcasecmp(leaf + n - 4, ".map") ? HOST_READ_LEVEL_MAP : HOST_READ_OTHER;
}
static uint32_t stat_attrs(struct stat *st) { uint32_t a = S_ISDIR(st->st_mode) ? 0x10 : 0x20; return a; }
static void fill_find_data(uint32_t fd_out, const char *hostpath, const char *leaf) {
    struct stat st; memset(GPTR(fd_out), 0, 320);
    if (stat(hostpath, &st) == 0) { S32(fd_out, stat_attrs(&st)); uint64_t ft = unix_to_filetime(st.st_mtime, 0); S64(fd_out + 4, ft); S64(fd_out + 12, ft); S64(fd_out + 20, ft); S32(fd_out + 28, (uint32_t)((uint64_t)st.st_size >> 32)); S32(fd_out + 32, (uint32_t)st.st_size); }
    snprintf((char *)GPTR(fd_out + 44), 260, "%s", leaf);
}
SHIM(CreateFileA) { const char *wp = GSTR(ARG(0)); uint32_t access = ARG(1), disp = ARG(4); char hp[1024]; host_path(wp, hp, sizeof hp);
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disp == 1) flags |= O_CREAT | O_EXCL; else if (disp == 2) flags |= O_CREAT | O_TRUNC; else if (disp == 4) flags |= O_CREAT; else if (disp == 5) flags |= O_TRUNC;
    int fd = open(hp, flags, 0644);
    if (getenv("HALO_FILELOG") && (strcasestr(wp,".map")||strcasestr(wp,"savegame")||strcasestr(wp,"a10"))) host_log("[file] open '%s' -> %s (%s)", wp, fd<0?"MISS":"ok", hp);
    if (fd < 0) { host_trace("[file] CreateFileA(%s) -> not found (%s)", wp, hp); host_set_last_error(errno == ENOENT ? 2 : 5); RET_STDCALL(0xFFFFFFFFu, 7); }
    FileObj *f = calloc(1, sizeof *f); f->fd = fd; f->kind = file_read_kind(wp); uint32_t h = host_handle_new(HANDLE_FILE, f);
    host_trace("[file] CreateFileA(%s, access %08X, disp %u) -> handle %08X", wp, access, disp, h); RET_STDCALL(h, 7); }
SHIM(CreateFileW) { uint32_t w = ARG(0); char name[512]; uint32_t i = 0; for (; i < 511 && G16(w + 2 * i); i++) name[i] = (char)G16(w + 2 * i); name[i] = 0;
    uint32_t tmp = guest_strdup(name); S32(cpu->gpr[4] + 4, tmp); shim_CreateFileA(cpu); }
typedef struct { uint32_t routine, overlapped, bytes; } Apc;
enum { APC_CAPACITY = 64 };
static _Thread_local Apc apc_queue[APC_CAPACITY];
static _Thread_local unsigned apc_head, apc_count;
static int deliver_apcs(EngineCPU *cpu) {
    int n = 0;
    /* Remove only the callback being invoked. A callback can enter another
     * alertable wait; that nested dispatch must see older queued completions
     * before any new operations the callback submits. */
    while (apc_count) {
        Apc apc = apc_queue[apc_head];
        apc_head = (apc_head + 1u) % APC_CAPACITY;
        --apc_count;
        uint32_t args[3] = { 0, apc.bytes, apc.overlapped };
        host_call_guest(cpu, apc.routine, 3, args, 1);
        /* The routine has marked its request done; wake a cache-read wait. */
        io_signal_completion();
        ++n;
    }
    return n;
}
static int64_t overlapped_offset(uint32_t ov) { return (int64_t)(((uint64_t)G32(ov + 12) << 32) | G32(ov + 8)); }
SHIM(ReadFile) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); uint32_t buf = ARG(1), n = ARG(2), outn = ARG(3), ov = ARG(4);
    if (!f) { host_set_last_error(6); RET_STDCALL(0, 5); }
    off_t _pos = ov ? overlapped_offset(ov) : lseek(f->fd, 0, SEEK_CUR);
    ssize_t r = ov ? pread(f->fd, GPTR(buf), n, overlapped_offset(ov)) : read(f->fd, GPTR(buf), n);
    if (r < 0) { host_set_last_error(5); RET_STDCALL(0, 5); }
    host_trace("[rd] h=%u off=%lld n=%u f4=%02x%02x%02x%02x", ARG(0), (long long)_pos, n, r>0?G8(buf):0, r>1?G8(buf+1):0, r>2?G8(buf+2):0, r>3?G8(buf+3):0);
    if (ov) { S32(ov, 0); S32(ov + 4, (uint32_t)r); lseek(f->fd, overlapped_offset(ov) + r, SEEK_SET); }
    if (outn) S32(outn, (uint32_t)r); RET_STDCALL(1, 5); }
SHIM(ReadFileEx) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); uint32_t buf = ARG(1), n = ARG(2), ov = ARG(3), routine = ARG(4);
    if (!f) { host_set_last_error(6); RET_STDCALL(0, 5); }
    if (!ov || !routine) { host_set_last_error(87); RET_STDCALL(0, 5); }
    /* Reject saturation before pread or writing either guest output. Every
     * accepted operation must retain its completion until alertable dispatch. */
    if (apc_count == APC_CAPACITY) { host_set_last_error(8); RET_STDCALL(0, 5); }
    ssize_t r;
    uint64_t read_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    do { r = pread(f->fd, GPTR(buf), n, overlapped_offset(ov)); } while (r < 0 && errno == EINTR);
    host_cache_read_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - read_start; host_cache_reads++;
    if (r < 0) { host_set_last_error(errno == EINVAL ? 87 : 5); RET_STDCALL(0, 5); }
    if (n && !r) { host_set_last_error(38); RET_STDCALL(0, 5); }
    S32(ov, 0); S32(ov + 4, (uint32_t)r);
    host_async_reads[f->kind]++; host_async_read_bytes[f->kind] += (uint64_t)r;
    apc_queue[(apc_head + apc_count++) % APC_CAPACITY] = (Apc){ routine, ov, (uint32_t)r };
    host_set_last_error(0);
    RET_STDCALL(1, 5); }
SHIM(WriteFile) { uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outn = ARG(3), ov = ARG(4);
    if (h == 0x104 || h == 0x108) { fwrite(GPTR(buf), 1, n, h == 0x104 ? stdout : stderr); if (outn) S32(outn, n); RET_STDCALL(1, 5); }
    FileObj *f = host_handle_object(h, HANDLE_FILE); if (!f) { host_set_last_error(6); RET_STDCALL(0, 5); }
    ssize_t r = ov ? pwrite(f->fd, GPTR(buf), n, overlapped_offset(ov)) : write(f->fd, GPTR(buf), n);
    if (ov && r >= 0) { S32(ov, 0); S32(ov + 4, (uint32_t)r); }
    if (outn) S32(outn, r < 0 ? 0 : (uint32_t)r); RET_STDCALL(r >= 0, 5); }
SHIM(SetFilePointer) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); int32_t lo = (int32_t)ARG(1); uint32_t hip = ARG(2), method = ARG(3);
    if (!f) { host_set_last_error(6); RET_STDCALL(0xFFFFFFFFu, 4); }
    int64_t off = hip ? (((int64_t)(int32_t)G32(hip)) << 32) | (uint32_t)lo : (int64_t)lo;
    off_t r = lseek(f->fd, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { host_set_last_error(131); RET_STDCALL(0xFFFFFFFFu, 4); }
    if (hip) S32(hip, (uint32_t)((uint64_t)r >> 32)); RET_STDCALL((uint32_t)r, 4); }
SHIM(GetFileSize) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); if (!f) RET_STDCALL(0xFFFFFFFFu, 2);
    struct stat st; fstat(f->fd, &st); if (ARG(1)) S32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32)); RET_STDCALL((uint32_t)st.st_size, 2); }
SHIM(SetEndOfFile) {
    FileObj *f = host_handle_object(ARG(0), HANDLE_FILE);
    if (!f) { host_set_last_error(6); RET_STDCALL(0, 1); }
    off_t end = lseek(f->fd, 0, SEEK_CUR);
    if (end < 0) { host_set_last_error(131); RET_STDCALL(0, 1); }
    int result;
    do { result = ftruncate(f->fd, end); } while (result && errno == EINTR);
    if (result) host_set_last_error(errno == ENOSPC || errno == EDQUOT ? 112u : 5u);
    RET_STDCALL(result == 0, 1);
}
SHIM(FlushFileBuffers) {
    FileObj *f = host_handle_object(ARG(0), HANDLE_FILE);
    if (!f) { host_set_last_error(6); RET_STDCALL(0, 1); }
    int result;
    do { result = fsync(f->fd); } while (result && errno == EINTR);
    if (result) host_set_last_error(errno == ENOSPC || errno == EDQUOT ? 112u : 5u);
    RET_STDCALL(result == 0, 1);
}
SHIM(GetFileTime) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); struct stat st; if (!f || fstat(f->fd, &st)) RET_STDCALL(0, 4);
    uint64_t ft = unix_to_filetime(st.st_mtime, 0); if (ARG(1)) S64(ARG(1), ft); if (ARG(2)) S64(ARG(2), ft); if (ARG(3)) S64(ARG(3), ft); RET_STDCALL(1, 4); }
SHIM(SetFileTime) { RET_STDCALL(1, 4); }
SHIM(CloseHandle) { uint32_t h = ARG(0); FileObj *f = host_handle_object(h, HANDLE_FILE); if (f) { close(f->fd); free(f); } host_handle_close(h); RET_STDCALL(1, 1); }
SHIM(GetFileAttributesA) { const char*wp=GSTR(ARG(0)); char hp[1024]; host_path(wp, hp, sizeof hp); struct stat st;
    int miss=stat(hp,&st);
    if (getenv("HALO_FILELOG") && strcasestr(wp,"a10")) host_log("[attr] GetFileAttributesA('%s') -> %s (%s)", wp, miss?"MISS":"ok", hp);
    if (miss) { host_set_last_error(2); RET_STDCALL(0xFFFFFFFFu, 1); } RET_STDCALL(stat_attrs(&st), 1); }
SHIM(GetFileAttributesExA) { char hp[1024]; host_path(GSTR(ARG(0)), hp, sizeof hp); struct stat st; uint32_t o = ARG(2);
    if (stat(hp, &st)) { host_set_last_error(2); RET_STDCALL(0, 3); }
    S32(o, stat_attrs(&st)); uint64_t ft = unix_to_filetime(st.st_mtime, 0); S64(o + 4, ft); S64(o + 12, ft); S64(o + 20, ft); S32(o + 28, (uint32_t)((uint64_t)st.st_size >> 32)); S32(o + 32, (uint32_t)st.st_size); RET_STDCALL(1, 3); }
SHIM(SetFileAttributesA) { RET_STDCALL(1, 2); }
typedef struct { glob_t g; size_t next; } FindObj;
SHIM(FindFirstFileA) { char hp[1024]; host_path(GSTR(ARG(0)), hp, sizeof hp); FindObj *fo = calloc(1, sizeof *fo);
    /* Win32's all-entry *.* pattern also matches extensionless names (MS-FSA
     * 2.1.4.4). POSIX glob requires a literal dot, hiding Halo's New001 profile
     * directories and making each restart look like the first launch. */
    char *leaf_pattern=strrchr(hp,'/');leaf_pattern=leaf_pattern?leaf_pattern+1:hp;
    if(!strcmp(leaf_pattern,"*.*"))leaf_pattern[1]='\0';
    if (getenv("HALO_FINDLOG")) host_log("[find] pattern='%s' host='%s'",GSTR(ARG(0)),hp);
    if (glob(hp, 0, NULL, &fo->g) || fo->g.gl_pathc == 0) { globfree(&fo->g); free(fo); host_set_last_error(2); host_trace("[file] FindFirstFileA(%s) -> none", GSTR(ARG(0))); RET_STDCALL(0xFFFFFFFFu, 2); }
    if (getenv("HALO_FINDLOG")) host_log("[find] matches=%zu first='%s'",fo->g.gl_pathc,fo->g.gl_pathv[0]);
    const char *p = fo->g.gl_pathv[0]; const char *leaf = strrchr(p, '/'); fill_find_data(ARG(1), p, leaf ? leaf + 1 : p); fo->next = 1;
    RET_STDCALL(host_handle_new(HANDLE_FIND, fo), 2); }
SHIM(FindNextFileA) { FindObj *fo = host_handle_object(ARG(0), HANDLE_FIND); if (!fo || fo->next >= fo->g.gl_pathc) { host_set_last_error(18); RET_STDCALL(0, 2); }
    const char *p = fo->g.gl_pathv[fo->next++]; const char *leaf = strrchr(p, '/'); fill_find_data(ARG(1), p, leaf ? leaf + 1 : p); RET_STDCALL(1, 2); }
SHIM(FindClose) { FindObj *fo = host_handle_object(ARG(0), HANDLE_FIND); if (fo) { globfree(&fo->g); free(fo); } host_handle_close(ARG(0)); RET_STDCALL(1, 1); }
SHIM(CreateDirectoryA) { char hp[1024]; host_path(GSTR(ARG(0)), hp, sizeof hp); int r = mkdir(hp, 0755); if (r && errno == EEXIST) host_set_last_error(183); RET_STDCALL(r == 0, 2); }
SHIM(RemoveDirectoryA) { char hp[1024]; host_path(GSTR(ARG(0)), hp, sizeof hp); RET_STDCALL(rmdir(hp) == 0, 1); }
SHIM(DeleteFileA) { char hp[1024]; host_path(GSTR(ARG(0)), hp, sizeof hp); RET_STDCALL(unlink(hp) == 0, 1); }
SHIM(CopyFileA) { char a[1024], b[1024]; host_path(GSTR(ARG(0)), a, sizeof a); host_path(GSTR(ARG(1)), b, sizeof b);
    FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL; if (!in || !out) { if (in) fclose(in); RET_STDCALL(0, 3); }
    char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out); fclose(in); fclose(out); RET_STDCALL(1, 3); }
SHIM(GetCurrentDirectoryA) { uint32_t n = copy_out(ARG(1), ARG(0), "C:\\Halo"); RET_STDCALL(n, 2); }
SHIM(GetFullPathNameA) { const char *name = GSTR(ARG(0)); char full[1024];
    if (isalpha((unsigned char)name[0]) && name[1] == ':') snprintf(full, sizeof full, "%s", name); else snprintf(full, sizeof full, "C:\\Halo\\%s", name);
    uint32_t n = copy_out(ARG(2), ARG(1), full); if (ARG(3)) { const char *bs = strrchr(full, '\\'); S32(ARG(3), ARG(2) + (uint32_t)(bs ? bs + 1 - full : 0)); } RET_STDCALL(n, 4); }
SHIM(GetTempPathA) { uint32_t n = copy_out(ARG(1), ARG(0), "C:\\Temp\\"); RET_STDCALL(n, 2); }
SHIM(GetDiskFreeSpaceExA) { if (ARG(1)) S64(ARG(1), UINT64_C(100) << 30); if (ARG(2)) S64(ARG(2), UINT64_C(500) << 30); if (ARG(3)) S64(ARG(3), UINT64_C(100) << 30); RET_STDCALL(1, 4); }
SHIM(CreateFileMappingA) { FileObj *f = host_handle_object(ARG(0), HANDLE_FILE); if (!f) RET_STDCALL(0, 6); FileObj *m = calloc(1, sizeof *m); m->fd = f->fd; RET_STDCALL(host_handle_new(HANDLE_MAPPING, m), 6); }
SHIM(MapViewOfFile) { FileObj *m = host_handle_object(ARG(0), HANDLE_MAPPING); if (!m) RET_STDCALL(0, 5); struct stat st; fstat(m->fd, &st); host_trace("[map] off=%u size=%u", ARG(3), ARG(4));
    uint32_t off = ARG(3), size = ARG(4); if (!size) size = (uint32_t)st.st_size - off; uint32_t a = guest_page_alloc(size); pread(m->fd, GPTR(a), size, off);
    host_trace("[file] MapViewOfFile -> %08X (%u bytes)", a, size); RET_STDCALL(a, 5); }
SHIM(UnmapViewOfFile) { RET_STDCALL(1, 1); }
int host_find_resource(uint32_t hinst, uint32_t type, uint32_t id, uint32_t *rva, uint32_t *size, const uint8_t **bytes);
typedef struct { uint32_t size; uint32_t guest; } ResObj;
static uint32_t find_resource_common(EngineCPU *cpu, uint32_t hinst, uint32_t name, uint32_t type) {
    if ((name & 0xFFFF0000u) || (type & 0xFFFF0000u)) { host_trace("[res] FindResource(type %08X name %08X) by string -> 0", type, name); return 0; }
    uint32_t rva, size; const uint8_t *bytes; if (!host_find_resource(hinst, type, name, &rva, &size, &bytes)) { host_trace("[res] FindResource(type %u id %u) -> not found", type, name); return 0; }
    ResObj *r = calloc(1, sizeof *r); r->size = size; r->guest = guest_alloc(size); memcpy(GPTR(r->guest), bytes, size);
    host_trace("[res] FindResource(type %u id %u) -> %u bytes", type, name, size); (void)cpu; return host_handle_new(HANDLE_MISC, r);
}
SHIM(FindResourceA) { RET_STDCALL(find_resource_common(cpu, ARG(0), ARG(1), ARG(2)), 3); }
SHIM(FindResourceExA) { RET_STDCALL(find_resource_common(cpu, ARG(0), ARG(2), ARG(1)), 4); }
SHIM(FindResourceW) { RET_STDCALL(0, 3); }
SHIM(LoadResource) { ResObj *r = host_handle_object(ARG(1), HANDLE_MISC); RET_STDCALL(r ? r->guest : 0, 2); }
SHIM(LockResource) { RET_STDCALL(ARG(0), 1); }
SHIM(SizeofResource) { ResObj *r = host_handle_object(ARG(1), HANDLE_MISC); RET_STDCALL(r ? r->size : 0, 2); }
SHIM(GetConsoleScreenBufferInfo) { RET_STDCALL(0, 2); }
SHIM(GetConsoleCursorInfo) { RET_STDCALL(0, 2); }
SHIM(SetConsoleCursorInfo) { RET_STDCALL(0, 2); }
SHIM(SetConsoleCursorPosition) { RET_STDCALL(0, 2); }
SHIM(GetNumberOfConsoleInputEvents) { RET_STDCALL(0, 2); }
SHIM(ReadConsoleInputA) { RET_STDCALL(0, 4); }
SHIM(WriteConsoleA) { fwrite(GPTR(ARG(1)), 1, ARG(2), stdout); if (ARG(3)) S32(ARG(3), ARG(2)); RET_STDCALL(1, 5); }
SHIM(WriteConsoleOutputCharacterA) { RET_STDCALL(0, 5); }
SHIM(FillConsoleOutputAttribute) { RET_STDCALL(0, 5); }
SHIM(FillConsoleOutputCharacterA) { RET_STDCALL(0, 5); }

/* ---- sync / threads ---- */
SHIM(CreateEventA) { RET_STDCALL(host_event_create(ARG(1) != 0, ARG(2) != 0), 4); }
SHIM(SetEvent) { int ok = host_event_set(ARG(0)); if (!ok) host_set_last_error(6); RET_STDCALL(ok, 1); }
SHIM(CreateMutexA) { RET_STDCALL(host_mutex_create(ARG(1) != 0), 3); }
SHIM(ReleaseMutex) { int ok = host_mutex_release(ARG(0)); if (!ok) host_set_last_error(288); RET_STDCALL(ok, 1); }
SHIM(WaitForSingleObject) { uint32_t result = host_wait_single(ARG(0), ARG(1)); if (result == HOST_WAIT_FAILED) host_set_last_error(6); RET_STDCALL(result, 2); }
SHIM(WaitForSingleObjectEx) { if (ARG(2) && deliver_apcs(cpu)) RET_STDCALL(0xC0, 3); uint32_t result = host_wait_single(ARG(0), ARG(1)); if (result == HOST_WAIT_FAILED) host_set_last_error(6); RET_STDCALL(result, 3); }
SHIM(DuplicateHandle) { if (ARG(3)) S32(ARG(3), ARG(1)); RET_STDCALL(1, 7); }
SHIM(CreateThread) { uint32_t tid = 0; uint32_t handle = host_thread_create(ARG(1), ARG(2), ARG(3), ARG(4), &tid); if (ARG(5)) S32(ARG(5), tid); if (!handle) host_set_last_error(8); RET_STDCALL(handle, 6); }
SHIM(ResumeThread) { uint32_t previous = host_thread_resume(ARG(0)); if (previous == 0xFFFFFFFFu) host_set_last_error(6); RET_STDCALL(previous, 1); }
SHIM(TerminateThread) { int ok = host_thread_terminate(ARG(0), ARG(1)); if (!ok) host_set_last_error(6); RET_STDCALL(ok, 2); }
SHIM(ExitThread) { uint32_t code = ARG(0); host_thread_exit(code); }
SHIM(GetExitCodeThread) { uint32_t code = 0; int ok = host_thread_get_exit_code(ARG(0), &code); if (ok && ARG(1)) S32(ARG(1), code); if (!ok) host_set_last_error(6); RET_STDCALL(ok, 2); }
SHIM(SetThreadPriority) { RET_STDCALL(1, 2); }
SHIM(GetThreadPriority) { RET_STDCALL(0, 1); }
SHIM(SetPriorityClass) { RET_STDCALL(1, 2); }
SHIM(GetPriorityClass) { RET_STDCALL(0x20, 1); }
SHIM(CreateProcessA) { RET_STDCALL(0, 10); }
SHIM(OutputDebugStringA) { host_log("[dbg] %s", ARG(0)?GSTR(ARG(0)):""); RET_STDCALL(0,1); }
SHIM(lstrcmpiA) { int r = strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); RET_STDCALL(r < 0 ? 0xFFFFFFFFu : r > 0 ? 1 : 0, 2); }

#define E(n) { "KERNEL32.dll", #n, shim_##n }
const HostShimEntry host_shims_kernel32[] = {
    E(GetModuleHandleA), E(GetModuleFileNameA), E(LoadLibraryA), E(FreeLibrary), E(GetProcAddress), E(GetCommandLineA), E(GetStartupInfoA),
    E(GetVersionExA), E(GetSystemInfo), E(GetEnvironmentStrings), E(GetEnvironmentStringsW), E(FreeEnvironmentStringsA), E(FreeEnvironmentStringsW), E(SetEnvironmentVariableA),
    E(GetStdHandle), E(SetStdHandle), E(GetFileType), E(SetHandleCount), E(GetCurrentProcess), E(GetCurrentThread), E(GetCurrentProcessId), E(GetCurrentThreadId),
    E(ExitProcess), E(TerminateProcess), E(SetUnhandledExceptionFilter), E(UnhandledExceptionFilter), E(SetErrorMode), E(GetLastError), E(SetLastError),
    E(IsProcessorFeaturePresent), E(IsBadReadPtr), E(IsBadWritePtr), E(IsBadCodePtr), E(RaiseException), E(RtlUnwind), E(FormatMessageA),
    E(HeapCreate), E(HeapDestroy), E(GetProcessHeap), E(HeapAlloc), E(HeapFree), E(HeapSize), E(HeapReAlloc), E(GlobalAlloc), E(GlobalFree), E(GlobalLock), E(GlobalUnlock), E(GlobalReAlloc),
    E(LocalAlloc), E(LocalFree), E(GlobalMemoryStatus), E(VirtualAlloc), E(VirtualFree), E(VirtualProtect), E(VirtualQuery), E(TlsAlloc), E(TlsFree), E(TlsGetValue), E(TlsSetValue),
    E(InitializeCriticalSection), E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection), E(InterlockedExchange),
    E(GetTickCount), E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetSystemTimeAsFileTime), E(GetSystemTime), E(GetLocalTime), E(GetTimeZoneInformation), E(SystemTimeToFileTime), E(CompareFileTime), E(Sleep), E(SleepEx), E(GetDateFormatA), E(GetTimeFormatA),
    E(GetACP), E(GetOEMCP), E(GetCPInfo), E(IsValidCodePage), E(IsValidLocale), E(GetUserDefaultLCID), E(GetThreadLocale), E(SetThreadLocale), E(GetLocaleInfoA), E(GetLocaleInfoW),
    E(GetStringTypeA), E(GetStringTypeW), E(LCMapStringA), E(LCMapStringW), E(MultiByteToWideChar), E(WideCharToMultiByte), E(CompareStringA), E(CompareStringW), E(EnumSystemLocalesA),
    E(CreateFileA), E(CreateFileW), E(ReadFile), E(ReadFileEx), E(WriteFile), E(SetFilePointer), E(GetFileSize), E(SetEndOfFile), E(FlushFileBuffers), E(GetFileTime), E(SetFileTime), E(CloseHandle),
    E(GetFileAttributesA), E(GetFileAttributesExA), E(SetFileAttributesA), E(FindFirstFileA), E(FindNextFileA), E(FindClose), E(CreateDirectoryA), E(RemoveDirectoryA), E(DeleteFileA), E(CopyFileA),
    E(GetCurrentDirectoryA), E(GetFullPathNameA), E(GetTempPathA), E(GetDiskFreeSpaceExA), E(CreateFileMappingA), E(MapViewOfFile), E(UnmapViewOfFile),
    E(FindResourceA), E(FindResourceExA), E(FindResourceW), E(LoadResource), E(LockResource), E(SizeofResource),
    E(GetConsoleScreenBufferInfo), E(GetConsoleCursorInfo), E(SetConsoleCursorInfo), E(SetConsoleCursorPosition), E(GetNumberOfConsoleInputEvents), E(ReadConsoleInputA), E(WriteConsoleA), E(WriteConsoleOutputCharacterA), E(FillConsoleOutputAttribute), E(FillConsoleOutputCharacterA),
    E(CreateEventA), E(SetEvent), E(CreateMutexA), E(ReleaseMutex), E(WaitForSingleObject), E(WaitForSingleObjectEx), E(DuplicateHandle), E(CreateThread), E(ResumeThread), E(TerminateThread), E(ExitThread), E(GetExitCodeThread),
    E(SetThreadPriority), E(GetThreadPriority), E(SetPriorityClass), E(GetPriorityClass), E(CreateProcessA), E(lstrcmpiA), E(OutputDebugStringA),
    { NULL, NULL, NULL }
};
