#include "../EngineReuse/engine_runtime.h"
#include "../EngineReuse/engine_cpu.h"
#include <stdio.h>
#include <string.h>

enum { ENTRY_SUCCESS = 0x1000, ENTRY_FAIL = 0x2000,
       ENTRY_READONLY = 0x3000, ENTRY_MISSING = 0x4000,
       ENTRY_BUDGET = 0x5000, ENTRY_FP_PUSH = 0x6000 };

/* ENGINE_STEP_FULL keeps bounded dispatch enabled in the canonical CPU. */
uint32_t engine_trace_lo, engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu) { (void)cpu; }

void engine_reuse_entry(EngineCPU *cpu, uint32_t entry) {
    switch (entry) {
    case ENTRY_SUCCESS: {
        uint32_t source = engine_read_u32(cpu, 0x1000);
        engine_write_u32(cpu, 0x2000, source + cpu->gpr[0]);
        cpu->gpr[0] = source + cpu->gpr[0];
        cpu->gpr[1] = 0x11223344;
        cpu->flags = 0xa5a5u;
        /* The x87 stack crosses the boundary in stack order, ST(0) first,
         * whatever the physical top: read what the caller left, push two. */
        if (cpu->fp_valid & 2) cpu->gpr[2] = (uint32_t)(engine_fp_read(cpu, 1) * 4);
        engine_fp_push(cpu, 1.5);
        engine_fp_push(cpu, 3.25);
        cpu->pc = entry + 4;
        return;
    }
    case ENTRY_FP_PUSH:
        engine_fp_push(cpu, 11.25);
        cpu->pc = entry + 4;
        return;
    case ENTRY_FAIL:
        engine_write_u32(cpu, 0x2000, 0xdeadbeef);
        engine_write_u16(cpu, 0x2008, 0xbeef);
        engine_write_u32(cpu, 0x2100, 0xcafebabe);
        cpu->gpr[0] = 0xffffffff;
        cpu->flags = 0xffffffff;
        cpu->pc = entry + 8;
        engine_fail(cpu, "intentional contract failure");
        return;
    case ENTRY_READONLY:
        engine_write_u32(cpu, 0x3000, 0xabcdef01);
        return;
    case ENTRY_MISSING:
        (void)engine_read_u32(cpu, 0x4000);
        return;
    case ENTRY_BUDGET:
        engine_step(cpu, entry);
        engine_step(cpu, entry + 1);
        engine_step(cpu, entry + 2);
        return;
    default:
        engine_fail(cpu, "unknown test entry");
        return;
    }
}

typedef struct { int checks; int failures; } Test;

static void check(Test *test, int condition, const char *name) {
    test->checks++;
    if (!condition) { test->failures++; fprintf(stderr, "FAIL %s\n", name); }
}

static void reject(Test *test, const HaloEngineRegion *regions, size_t count,
                   const char *name) {
    HaloEngineResult result = {0};
    HaloEngineRuntime *runtime = halo_engine_create(regions, count, &result);
    check(test, runtime == NULL && result.status == HALO_ENGINE_INVALID, name);
    halo_engine_destroy(runtime);
}

int main(void) {
    Test test = {0};
    uint8_t code[0x20] = {0};
    uint8_t mutable_bytes[0x20] = {0};
    uint8_t mutable_aux[0x10] = {0};
    uint8_t readonly_bytes[0x10] = {0x5a};
    uint32_t source = 0x01020304;
    memcpy(code, &source, sizeof source);
    HaloEngineRegion regions[] = {
        {0x1000, (uint32_t)sizeof code, code, 0},
        {0x2000, (uint32_t)sizeof mutable_bytes, mutable_bytes, 1},
        {0x2100, (uint32_t)sizeof mutable_aux, mutable_aux, 1},
        {0x3000, (uint32_t)sizeof readonly_bytes, readonly_bytes, 0},
    };
    HaloEngineResult create_result = {0};
    HaloEngineRuntime *runtime = halo_engine_create(regions, sizeof regions / sizeof regions[0], &create_result);
    check(&test, runtime != NULL && create_result.status == HALO_ENGINE_OK, "create explicit regions");
    if (!runtime) return 1;

    /* Creation owns a copy; changing caller storage cannot alter guest memory. */
    code[0] = 0xff;
    uint32_t readback = 0;
    check(&test, halo_engine_read(runtime, 0x1000, &readback, sizeof readback) == HALO_ENGINE_OK &&
          readback == source, "owned source copy and read hook");
    uint8_t write_value = 0x77;
    check(&test, halo_engine_write(runtime, 0x3000, &write_value, 1) == HALO_ENGINE_INVALID,
          "readonly public write rejected");
    uint8_t readonly_read = 0;
    check(&test, halo_engine_read(runtime, 0x3000, &readonly_read, 1) == HALO_ENGINE_OK &&
          readonly_read == 0x5a, "readonly data unchanged");
    check(&test, halo_engine_read(runtime, 0x3010, &readback, 1) == HALO_ENGINE_INVALID,
          "unmapped boundary read rejected");
    check(&test, halo_engine_write(runtime, 0x201f, &write_value, 2) == HALO_ENGINE_INVALID,
          "cross-region write rejected");

    HaloEngineState state = {0};
    state.registers[0] = 7;
    state.flags = 3;
    state.pc = 0xaaaa;
    state.fp_control = 0x037f;
    state.fp_top = 0;
    HaloEngineResult result = halo_engine_invoke(runtime, ENTRY_SUCCESS, &state);
    uint32_t expected = source + 7;
    check(&test, result.status == HALO_ENGINE_OK && result.pc == ENTRY_SUCCESS + 4,
          "successful dispatch result");
    check(&test, state.registers[0] == expected && state.registers[1] == 0x11223344 &&
          state.flags == 0xa5a5 && state.pc == ENTRY_SUCCESS + 4 &&
          state.fp_valid == 3 && state.fp_top == 6 && state.fp_values[0] == 3.25 &&
          state.fp_values[1] == 1.5, "CPU state publishing");
    check(&test, halo_engine_read(runtime, 0x2000, &readback, sizeof readback) == HALO_ENGINE_OK &&
          readback == expected, "successful mutable write hook");

    uint8_t mutable_before[sizeof mutable_bytes];
    uint8_t mutable_aux_before[sizeof mutable_aux];
    check(&test, halo_engine_read(runtime, 0x2000, mutable_before, sizeof mutable_before) == HALO_ENGINE_OK,
          "capture mutable transaction state");
    check(&test, halo_engine_read(runtime, 0x2100, mutable_aux_before, sizeof mutable_aux_before) == HALO_ENGINE_OK,
          "capture second mutable transaction state");
    HaloEngineState failed_state = state, failed_before = state;
    result = halo_engine_invoke(runtime, ENTRY_FAIL, &failed_state);
    uint8_t mutable_after[sizeof mutable_bytes];
    uint8_t mutable_aux_after[sizeof mutable_aux];
    check(&test, result.status == HALO_ENGINE_EXECUTION &&
          strstr(result.reason, "intentional contract failure") != NULL, "intentional engine_fail");
    check(&test, memcmp(&failed_state, &failed_before, sizeof failed_state) == 0,
          "CPU rollback after failure");
    check(&test, halo_engine_read(runtime, 0x2000, mutable_after, sizeof mutable_after) == HALO_ENGINE_OK &&
          halo_engine_read(runtime, 0x2100, mutable_aux_after, sizeof mutable_aux_after) == HALO_ENGINE_OK &&
          memcmp(mutable_after, mutable_before, sizeof mutable_after) == 0 &&
          memcmp(mutable_aux_after, mutable_aux_before, sizeof mutable_aux_after) == 0,
          "full mutable memory rollback after failure");

    result = halo_engine_invoke(runtime, ENTRY_MISSING, &state);
    check(&test, result.status == HALO_ENGINE_EXECUTION &&
          strstr(result.reason, "unmapped guest address") != NULL, "missing address failure");
    result = halo_engine_invoke(runtime, ENTRY_READONLY, &state);
    check(&test, result.status == HALO_ENGINE_EXECUTION &&
          strstr(result.reason, "readonly") != NULL, "readonly engine write failure");
    HaloEngineState budget_state = state, budget_before = state;
    result = halo_engine_invoke_bounded(runtime, ENTRY_BUDGET, &budget_state, 2);
    check(&test, result.status == HALO_ENGINE_EXECUTION &&
          strstr(result.reason, "instruction budget") != NULL &&
          memcmp(&budget_state, &budget_before, sizeof budget_state) == 0,
          "bounded instruction failure rolls back CPU state");
    check(&test, halo_engine_read(runtime, 0x2000, mutable_after, sizeof mutable_after) == HALO_ENGINE_OK &&
          memcmp(mutable_after, mutable_before, sizeof mutable_after) == 0,
          "bounded instruction failure preserves prior successful memory");
    HaloEngineState zero_budget_state = state, zero_budget_before = state;
    result = halo_engine_invoke_bounded(runtime, ENTRY_BUDGET, &zero_budget_state, 0);
    check(&test, result.status == HALO_ENGINE_INVALID &&
          memcmp(&zero_budget_state, &zero_budget_before, sizeof zero_budget_state) == 0,
          "zero instruction budget rejected unchanged");
    result = halo_engine_invoke(runtime, ENTRY_SUCCESS, &state);
    check(&test, result.status == HALO_ENGINE_OK && state.registers[0] == source + expected &&
          state.registers[2] == 6 && state.fp_valid == 15 && state.fp_top == 4 &&
          state.fp_values[0] == 3.25 && state.fp_values[1] == 1.5 && state.fp_values[2] == 3.25 &&
          state.fp_values[3] == 1.5, "repeat success after failures");
    HaloEngineState invalid_state = state;
    invalid_state.fp_control = 0;
    HaloEngineState invalid_before = invalid_state;
    result = halo_engine_invoke(runtime, ENTRY_SUCCESS, &invalid_state);
    check(&test, result.status == HALO_ENGINE_INVALID &&
          memcmp(&invalid_state, &invalid_before, sizeof invalid_state) == 0, "invalid x87 state rejected unchanged");

    /* Cross the public boundary at every TOP with sparse valid masks. Compare
     * all logical slots, even empty ones: FLDENV can make those live later. */
    for (unsigned top = 0; top < 8; top++) {
        for (unsigned valid = 0; valid < 128; valid++) {
            HaloEngineState input = {0};
            input.fp_top = (uint8_t)top;
            input.fp_valid = (uint8_t)valid;
            input.fp_control = 0x037f;
            input.fp_status = 0x4100;
            for (unsigned i = 0; i < 8; i++) input.fp_values[i] = (double)i - 3.125;
            HaloEngineState expected_fp = input;
            expected_fp.fp_top = (uint8_t)((top - 1u) & 7u);
            expected_fp.fp_valid = (uint8_t)((valid << 1) | 1u);
            for (unsigned i = 7; i; i--) expected_fp.fp_values[i] = input.fp_values[i - 1];
            expected_fp.fp_values[0] = 11.25;
            result = halo_engine_invoke(runtime, ENTRY_FP_PUSH, &input);
            check(&test, result.status == HALO_ENGINE_OK && input.pc == ENTRY_FP_PUSH + 4 &&
                  input.fp_top == expected_fp.fp_top && input.fp_valid == expected_fp.fp_valid &&
                  input.fp_control == expected_fp.fp_control && input.fp_status == expected_fp.fp_status &&
                  memcmp(input.fp_values, expected_fp.fp_values, sizeof input.fp_values) == 0,
                  "logical x87 boundary across TOP and sparse tags");
        }
    }

    HaloEngineRegion bad_zero = {0x10, 0, code, 0};
    reject(&test, &bad_zero, 1, "zero length rejected");
    HaloEngineRegion bad_write_flag = {0x10, 1, code, 2};
    reject(&test, &bad_write_flag, 1, "invalid writable flag rejected");
    HaloEngineRegion bad_overflow = {0xfffffff0u, 0x20, code, 0};
    reject(&test, &bad_overflow, 1, "guest address overflow rejected");
    HaloEngineRegion overlap[] = {{0x5000, 8, code, 0}, {0x5004, 8, code, 1}};
    reject(&test, overlap, 2, "overlapping regions rejected");
    reject(&test, NULL, 0, "missing region descriptors rejected");

    halo_engine_destroy(runtime);
    printf("PASS engine runtime contract: %d checks\n", test.checks);
    return test.failures ? 1 : 0;
}
