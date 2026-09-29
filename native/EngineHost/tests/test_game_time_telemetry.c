/* Run the original translated tick driver to verify the telemetry's observed
 * game-time field against actual calls to game_tick. The driver is read-only
 * generated source; only its callees are recorded. No game files are needed. */
#if !__has_include("sub_00470BF0.c")
#include <stdio.h>
int main(void) { puts("SKIP: generated sub_00470BF0.c is not on the include path"); return 0; }
#else
#include "host.h"
#include "core_telemetry.h"
#include "engine_flags.h"
#include "engine_functions.h"
#include "engine_registers.h"
#include "sub_00470BF0.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags
#include <assert.h>
#include <stdio.h>

uint8_t *engine_flat_base;
enum { MEMORY_SIZE = 0x800000, GLOBALS = 0x750000, STACK = 0x10000, RETURN_PC = 0x12345678 };
static uint32_t requested_ticks, recorded_ticks;
static unsigned checked_cases;
static uint8_t *memory_snapshot;

static void returned(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }
void sub_00470B30(EngineCPU *cpu) { cpu->gpr[0] = requested_ticks; returned(cpu); }
void sub_00473310(EngineCPU *cpu) { returned(cpu); }
void sub_004E03C0(EngineCPU *cpu) { returned(cpu); }
void sub_0045B4F0(EngineCPU *cpu) { returned(cpu); }
void sub_0045B780(EngineCPU *cpu) { recorded_ticks++; returned(cpu); }
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    (void)cpu; fprintf(stderr, "unexpected intercepted call %08x\n", address); abort();
}

static HaloFrameSample observed(const EngineCPU *cpu) {
    EngineCPU cpu_before = *cpu;
    memcpy(memory_snapshot, engine_flat_base, MEMORY_SIZE);
    HaloFrameSample sample = {0};
    halo_game_time_read(engine_flat_base, &sample);
    /* Counter telemetry never changes guest memory or observable CPU/x87. */
    assert(!memcmp(&cpu_before, cpu, sizeof *cpu));
    assert(!memcmp(memory_snapshot, engine_flat_base, MEMORY_SIZE));
    assert(sample.ticks_valid && sample.tick_globals == GLOBALS);
    return sample;
}

static void tick_driver(uint32_t requested, unsigned game_mode, int active, int force_one) {
    memset(engine_flat_base, 0, MEMORY_SIZE);
    S32(HALO_GAME_TIME_GLOBALS, GLOBALS);
    S32(GLOBALS + 0x0C, 1234); S32(GLOBALS + 0x14, 4321);
    S16(GLOBALS + 0x10, 99); S8(GLOBALS + 1, active);
    S16(0x719720, game_mode); S32(0x7196D8, force_one);
    EngineCPU cpu = {0}; cpu.gpr[4] = STACK; cpu.pc = 0x00470BF0;
    cpu.fp_control = 0x027F; cpu.flags = 0x202;
    S32(STACK, RETURN_PC);
    requested_ticks = requested; recorded_ticks = 0;
    HaloFrameSample before = observed(&cpu);
    sub_00470BF0(&cpu);
    assert(cpu.pc == RETURN_PC && cpu.gpr[4] == STACK + 4);
    HaloFrameSample after = observed(&cpu);
    int resync;
    uint32_t delta = halo_frame_ticks(&before, &after, &resync);
    uint32_t expected = !game_mode && !active ? 0 : force_one ? 1 : requested;
    assert(!resync && recorded_ticks == expected && delta == recorded_ticks);
    assert(G32(GLOBALS + 0x14) - 4321 == recorded_ticks);
    assert(after.tick_last_call == recorded_ticks); /* inactive driver zeroes +0x10 */
    checked_cases++;
}

int main(void) {
    engine_flat_base = calloc(1, MEMORY_SIZE); memory_snapshot = malloc(MEMORY_SIZE);
    assert(engine_flat_base && memory_snapshot);
    const uint32_t ticks[] = {0, 1, 2, 3, 4, 7, 120};
    for (unsigned mode = 0; mode <= 2; mode++)
        for (unsigned i = 0; i < sizeof ticks / sizeof *ticks; i++) tick_driver(ticks[i], mode, 1, 0);
    tick_driver(4, 0, 0, 0);
    tick_driver(4, 0, 1, 1);
    /* If no driver runs between samples, +0x10 is the previous call's value;
     * the counter delta still correctly reports no ticks. */
    EngineCPU cpu = {0};
    HaloFrameSample before = observed(&cpu), after = observed(&cpu);
    int resync;
    assert(after.tick_last_call == 1 && halo_frame_ticks(&before, &after, &resync) == 0 && !resync);
    free(memory_snapshot); free(engine_flat_base);
    printf("PASS game-time telemetry: %u translated-driver cases, 0-120 ticks, all 3 game modes, inactive/forced ticks, stale last-call, read-only guest/CPU/x87\n", checked_cases);
    return 0;
}
#endif
