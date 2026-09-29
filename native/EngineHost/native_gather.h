/* Exact native BSP surface-gather loops. The input volumes and current view's
 * visible-triangle bitset are read on every call; no result survives a call.
 *
 * 005540C0 enumerates a compact BSP leaf; 00553C40 enumerates cluster groups.
 * Keep the original load/store order, including redundant loads, argument
 * slots, saved registers, child return addresses, and abandoned stack locals.
 * This matters when output aliases inputs, and to an exact translated-engine
 * comparison. Only the integer flag calculations whose results are killed by
 * the final add esp are omitted. Calls with fewer than four free x87 slots
 * decline untouched, so an overflow takes the full translated exception path.
 * Bounded diagnostic execution also stays entirely in the translated path.
 * The bounds children retain their established
 * exact native implementations, with translated dispatch for their declined
 * cases (rounding mode, exhausted x87 stack, or overlapping scratch).
 */
#ifndef HALO_NATIVE_GATHER_H
#define HALO_NATIVE_GATHER_H
#include "native_leaves.h"

static int gather_call_bounds(EngineCPU *cpu, uint32_t address, uint32_t ret) {
    engine_push(cpu, ret, 4);
    cpu->pc = address;
    int done = address == 0x00553380u ? leaf_bsp_node_bounds(cpu) :
               address == 0x005541B0u ? leaf_bounds_overlap(cpu) : leaf_bounds_planes(cpu);
    if (!done) engine_dispatch(cpu, address);
    return cpu->pc == ret;
}

#define NG_A cpu->gpr[0]
#define NG_C cpu->gpr[1]
#define NG_D cpu->gpr[2]
#define NG_B cpu->gpr[3]
#define NG_E cpu->gpr[4]
#define NG_P cpu->gpr[5]
#define NG_S cpu->gpr[6]
#define NG_I cpu->gpr[7]

static int gather_native_leaf(EngineCPU *cpu) {
    uint32_t entry_sp = NG_E;
    if (cpu->instruction_limit || (cpu->fp_valid & 0xF0u) || entry_sp < 0x70u || !leaf_span(entry_sp, 0x20u)) return 0;
    NG_E -= 0x1Cu;
    engine_push(cpu, NG_B, 4); engine_push(cpu, NG_P, 4);
    engine_push(cpu, NG_S, 4); engine_push(cpu, NG_I, 4);
    NG_I = NG_C;
    NG_C = G32(0x00746F9Cu);
    NG_D = G32(NG_C + 0xE4u);
    NG_C = G32(NG_E + 0x30u);
    NG_A = ((NG_A & 0x7FFFFFFFu) << 4) + NG_D;
    NG_B = NG_A;
    NG_S = NG_E + 0x14u;
    NG_D = NG_B;
    NG_P = engine_flags_logic(&cpu->flags, 0, 32u);
    S32(NG_E + 0x10u, NG_B);
    if (!gather_call_bounds(cpu, 0x00553380u, 0x005540F6u)) return 1;
    if ((uint16_t)NG_I != 2u) {
        NG_C = G32(NG_E + 0x40u);
        NG_D = NG_S;
        if (!gather_call_bounds(cpu, 0x005541B0u, 0x00554107u)) return 1;
        NG_B = G32(NG_E + 0x48u);
        NG_I = G32(NG_E + 0x44u);
        NG_S = NG_A;
        NG_A = NG_D;
        if (!gather_call_bounds(cpu, 0x00554260u, 0x00554118u)) return 1;
        NG_B = G32(NG_E + 0x10u);
        NG_I = NG_A;
        if ((int16_t)NG_S <= (int16_t)NG_A) NG_I = NG_S;
    }
    if ((uint16_t)NG_I) {
        NG_A = (uint32_t)(int32_t)(int16_t)G16(NG_B + 0xAu);
        NG_I = G32(NG_B + 0xCu);
        NG_A += NG_I;
        if ((int32_t)NG_I < (int32_t)NG_A) {
            do {
                NG_A = G32(0x00746F9Cu);
                NG_C = G32(NG_A + 0xF0u);
                NG_S = G32(NG_C + NG_I * 8u);
                NG_C = NG_S & 31u;
                NG_A = (uint32_t)((int32_t)NG_S >> 5) << 2;
                NG_D = 1u << NG_C;
                if (G32(NG_A + 0x007D0394u) & NG_D) {
                    NG_C = G32(NG_E + 0x34u);
                    if (!(G32(NG_A + NG_C) & NG_D)) {
                        if ((int16_t)NG_P >= (int16_t)G16(NG_E + 0x3Cu)) break;
                        S32(NG_A + NG_C, G32(NG_A + NG_C) | NG_D);
                        NG_A = G32(NG_E + 0x38u);
                        NG_D = (uint32_t)(int32_t)(int16_t)NG_P;
                        S32(NG_A + NG_D * 4u, NG_S);
                        NG_P++;
                    }
                }
                NG_C = (uint32_t)(int32_t)(int16_t)G16(NG_B + 0xAu);
                NG_D = G32(NG_B + 0xCu);
                NG_I++;
                NG_C += NG_D;
            } while ((int32_t)NG_I < (int32_t)NG_C);
        }
    }
    NG_I = engine_pop(cpu, 4); NG_S = engine_pop(cpu, 4);
    NG_A = (NG_A & 0xFFFF0000u) | (uint16_t)NG_P;
    NG_P = engine_pop(cpu, 4); NG_B = engine_pop(cpu, 4);
    NG_E = engine_flags_add(&cpu->flags, NG_E, 0x1Cu, 0, 32u);
    leaf_return(cpu);
    return 1;
}

static int gather_native_clusters(EngineCPU *cpu) {
    uint32_t entry_sp = NG_E;
    if (cpu->instruction_limit || (cpu->fp_valid & 0xF0u) || entry_sp < 0x60u || !leaf_span(entry_sp, 0x24u)) return 0;
    NG_E -= 0x10u;
    NG_A = 0;
    engine_push(cpu, NG_P, 4);
    NG_P = 0;
    /* cmp precedes this store in the original. An argument can alias it. */
    int more_clusters = (int16_t)G16(NG_E + 0x30u) > 0;
    S32(NG_E + 0xCu, NG_A);
    if (more_clusters) {
        engine_push(cpu, NG_B, 4); engine_push(cpu, NG_S, 4); engine_push(cpu, NG_I, 4);
        do {
            if ((int16_t)NG_P >= (int16_t)G16(NG_E + 0x28u)) break;
            NG_C = G32(NG_E + 0x40u);
            NG_A = (uint32_t)(int32_t)(int16_t)NG_A;
            NG_S = (uint32_t)(int32_t)(int16_t)G16(NG_C + NG_A * 2u);
            NG_A = G32(0x00746F9Cu);
            NG_S *= 0x68u;
            NG_D = G32(NG_A + 0x138u);
            NG_C = G32(NG_S + NG_D + 0x34u);
            NG_S += NG_D;
            NG_A = 0;
            S32(NG_E + 0x1Cu, NG_S); S32(NG_E + 0x14u, NG_A);
            if ((int32_t)NG_C > 0) {
                do {
                    if ((int16_t)NG_P >= (int16_t)G16(NG_E + 0x28u)) break;
                    NG_A = (uint32_t)(int32_t)(int16_t)NG_A;
                    NG_D = NG_A * 9u;
                    NG_A = G32(NG_S + 0x38u);
                    NG_C = NG_A + NG_D * 4u;
                    NG_D = G32(NG_E + 0x2Cu);
                    S32(NG_E + 0x10u, NG_C);
                    if (!gather_call_bounds(cpu, 0x005541B0u, 0x00553CC4u)) return 1;
                    if (!(uint16_t)NG_A) goto next_group;
                    NG_B = G32(NG_E + 0x34u); NG_I = G32(NG_E + 0x30u);
                    NG_A = NG_C;
                    if (!gather_call_bounds(cpu, 0x00554260u, 0x00553CD8u)) return 1;
                    if (!(uint16_t)NG_A) goto next_group;
                    NG_C = G32(NG_E + 0x10u);
                    NG_S = G32(NG_C + 0x1Cu);
                    NG_B = NG_C;
                    NG_A = G32(NG_B + 0x18u);
                    NG_I = 0;
                    if ((int32_t)NG_A > 0) {
                        do {
                            NG_A = G32(NG_S);
                            NG_C = NG_A & 31u;
                            NG_D = 1u << NG_C;
                            NG_A = (uint32_t)((int32_t)NG_A >> 5) << 2;
                            if (G32(NG_A + 0x007D0394u) & NG_D) {
                                NG_C = G32(NG_E + 0x38u);
                                if (!(G32(NG_A + NG_C) & NG_D)) {
                                    if ((int16_t)NG_P >= (int16_t)G16(NG_E + 0x28u)) break;
                                    S32(NG_A + NG_C, G32(NG_A + NG_C) | NG_D);
                                    NG_A = G32(NG_S);
                                    NG_C = G32(NG_E + 0x24u);
                                    NG_D = (uint32_t)(int32_t)(int16_t)NG_P;
                                    S32(NG_C + NG_D * 4u, NG_A);
                                    NG_P++;
                                }
                            }
                            NG_A = G32(NG_B + 0x18u);
                            NG_S += 4u; NG_I++;
                            NG_D = (uint32_t)(int32_t)(int16_t)NG_I;
                        } while ((int32_t)NG_D < (int32_t)NG_A);
                    }
                    NG_S = G32(NG_E + 0x1Cu);
next_group:
                    NG_A = G32(NG_E + 0x14u);
                    NG_D = G32(NG_S + 0x34u);
                    NG_A++;
                    NG_C = (uint32_t)(int32_t)(int16_t)NG_A;
                    int more_groups = (int32_t)NG_C < (int32_t)NG_D;
                    S32(NG_E + 0x14u, NG_A);
                    if (!more_groups) break;
                } while (1);
            }
            NG_A = G32(NG_E + 0x18u);
            NG_A++;
            more_clusters = (int16_t)NG_A < (int16_t)G16(NG_E + 0x3Cu);
            S32(NG_E + 0x18u, NG_A);
        } while (more_clusters);
        NG_I = engine_pop(cpu, 4); NG_S = engine_pop(cpu, 4); NG_B = engine_pop(cpu, 4);
    }
    NG_A = (NG_A & 0xFFFF0000u) | (uint16_t)NG_P;
    NG_P = engine_pop(cpu, 4);
    NG_E = engine_flags_add(&cpu->flags, NG_E, 0x10u, 0, 32u);
    leaf_return(cpu);
    return 1;
}
#undef NG_A
#undef NG_C
#undef NG_D
#undef NG_B
#undef NG_E
#undef NG_P
#undef NG_S
#undef NG_I
#endif
