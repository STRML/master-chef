/* Exact native 00553F10, the recursive collision-BSP surface gather.
 *
 * This retains the translated routine's traversal order, float operation
 * order, every guest stack store (including popped argument slots), integer
 * registers and observable x87 state. It removes interpreter bookkeeping and
 * intermediate integer flags that cannot reach an observer. All children use
 * the same guest stack and original return address; a declined native child
 * is dispatched normally. No geometry or visibility is cached across calls.
 * Include after native_gather.h, which supplies the exact leaf gather.
 */
#ifndef HALO_NATIVE_GATHER_TREE_H
#define HALO_NATIVE_GATHER_TREE_H
#include "native_gather.h"

static int gather_native_tree(EngineCPU *cpu);

static int gather_tree_call(EngineCPU *cpu, uint32_t address, uint32_t next) {
    engine_push(cpu, next, 4);
    cpu->pc = address;
    int done;
    switch (address) {
    case 0x00553380u: done = leaf_bsp_node_bounds(cpu); break;
    case 0x005541B0u: done = leaf_bounds_overlap(cpu); break;
    case 0x00554260u: done = leaf_bounds_planes(cpu); break;
    case 0x005540C0u: done = gather_native_leaf(cpu); break;
    case 0x00553F10u: done = gather_native_tree(cpu); break;
    default: done = 0; break;
    }
    if (!done) engine_dispatch(cpu, address);
    return cpu->pc == next;
}

/* Register names are deliberately local to this implementation. */
#define GTA (cpu->gpr[0])
#define GTC (cpu->gpr[1])
#define GTD (cpu->gpr[2])
#define GTB (cpu->gpr[3])
#define GTS (cpu->gpr[4])
#define GTP (cpu->gpr[5])
#define GTI (cpu->gpr[6])
#define GTT (cpu->gpr[7])
#define GT_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))

static int gather_native_tree(EngineCPU *cpu) {
    /* Honor bounded diagnostic execution in the translated routine. */
    if (cpu->instruction_limit || GTS < 0x100u || !leaf_span(GTS, 0x30u) ||
        (cpu->fp_valid & 0xF0u)) return 0;
    GTS -= 0x20u;
    GTA = G32(0x00746F9Cu);
    GTC = G32(GTA + 0xB4u);
    GTA = G32(GTA + 0xC0u);
    engine_push(cpu, GTB, 4);
    GTB = G32(GTS + 0x28u);
    engine_push(cpu, GTP, 4);
    engine_push(cpu, GTI, 4);
    S32(GTS + 0x10u, GTC);
    GTC = G32(GTS + 0x34u);
    GTD = GTB + GTB * 2u;
    engine_push(cpu, GTT, 4);
    GTD = GTA + GTD * 2u;
    GTI = GTS + 0x18u;
    GTP = 0;
    /* xor ebp,ebp is the flags input to the first child. */
    (void)engine_flags_logic(&cpu->flags, 0, 32u);
    if (!gather_tree_call(cpu, 0x00553380u, 0x00553F45u)) return 1;

    GTT = G32(GTS + 0x5Cu);
    if ((uint16_t)GTT != 2u) {
        GTC = G32(GTS + 0x50u);
        GTD = GTI;
        (void)engine_flags_sub(&cpu->flags, (uint16_t)GTT, 2u, 0u, 16u);
        if (!gather_tree_call(cpu, 0x005541B0u, 0x00553F5Au)) return 1;
        GTI = GTA;
        if ((uint16_t)GTI) {
            GTB = G32(GTS + 0x58u);
            GTT = G32(GTS + 0x54u);
            GTA = GTD;
            (void)engine_flags_logic(&cpu->flags, (uint16_t)GTI, 16u);
            if (!gather_tree_call(cpu, 0x00554260u, 0x00553F70u)) return 1;
            if ((uint16_t)GTA == 2u) S32(GTS + 0x54u, GTP);
            GTB = G32(GTS + 0x34u);
            GTT = (int16_t)GTI > (int16_t)GTA ? GTA : GTI;
        } else {
            GTT = GTI;
        }
    }
    if (!(uint16_t)GTT) goto gather_tree_return;

    GTD = G32(GTS + 0x14u);
    GTA = G32(GTD + 4u);
    GTI = G32(GTD + 0x10u);
    GTD = G32(GTS + 0x48u);
    GTC = GTB + GTB * 2u;
    GTC = GTA + GTC * 4u;
    GTA = G32(GTC) << 4;
    engine_fp_push(cpu, engine_read_f32(cpu, GTA + GTI + 8u));
    GTA += GTI;
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL,
                    engine_fp_read(cpu, 0), engine_read_f32(cpu, GTD + 8u)));
    engine_fp_push(cpu, engine_read_f32(cpu, GTA + 4u));
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL,
                    engine_fp_read(cpu, 0), engine_read_f32(cpu, GTD + 4u)));
    engine_fp_write(cpu, 1, engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                    engine_fp_read(cpu, 1), engine_fp_read(cpu, 0)));
    engine_fp_pop(cpu);
    engine_fp_push(cpu, engine_read_f32(cpu, GTD));
    GTD = (GTD & 0xFFFFFF00u) | 1u;
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL,
                    engine_fp_read(cpu, 0), engine_read_f32(cpu, GTA)));
    S8(GTS + 0x34u, (uint8_t)GTD);
    engine_fp_write(cpu, 1, engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                    engine_fp_read(cpu, 1), engine_fp_read(cpu, 0)));
    engine_fp_pop(cpu);
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_SUB,
                    engine_fp_read(cpu, 0), engine_read_f32(cpu, GTA + 12u)));
    engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_read_f32(cpu, GTS + 0x4Cu), 0);
    GT_LO16(GTA, engine_fp_status(cpu));
    /* test ah,5 / jnp: retain front only for ordered radius > distance. */
    if ((((GTA >> 8) & 5u) != 1u) && (((GTA >> 8) & 5u) != 4u)) S8(GTS + 0x34u, 0);
    engine_fp_push(cpu, engine_read_f32(cpu, GTS + 0x4Cu));
    S8(GTS + 0x35u, (uint8_t)GTD);
    engine_fp_unary(cpu, ENGINE_FP_FCHS);
    engine_fp_exchange(cpu, 1);
    engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_fp_read(cpu, 1), 0);
    engine_fp_pop(cpu);
    engine_fp_pop(cpu);
    GT_LO16(GTA, engine_fp_status(cpu));
    if ((GTA >> 8) & 0x41u) S8(GTS + 0x35u, 0);
    GTB = G32(GTS + 0x40u);
    GTI = G32(GTS + 0x3Cu);
    GTD = GTS + 0x34u;
    GTC += 4u;
    S32(GTS + 0x5Cu, GTD);
    S32(GTS + 0x10u, GTC);
    S32(GTS + 0x14u, 2u);

    do {
        GTA = G32(GTS + 0x5Cu);
        if (G8(GTA)) {
            GTA = G32(GTC);
            if ((int32_t)GTA >= 0) {
                GTC = G32(GTS + 0x58u);
                GTD = G32(GTS + 0x54u);
                engine_push(cpu, GTT, 4);
                engine_push(cpu, GTC, 4);
                GTC = G32(GTS + 0x58u);
                engine_push(cpu, GTD, 4);
                GTD = G32(GTS + 0x58u);
                engine_push(cpu, GTC, 4);
                GTC = G32(GTS + 0x58u);
                engine_push(cpu, GTD, 4);
                GTD = G32(GTS + 0x58u);
                engine_push(cpu, GTC, 4);
                GTD = engine_flags_sub(&cpu->flags, GTD, GTP, 0u, 32u);
                engine_push(cpu, GTD, 4);
                GTC = (uint32_t)(int32_t)(int16_t)GTP;
                GTD = GTB + GTC * 4u;
                engine_push(cpu, GTD, 4);
                engine_push(cpu, GTI, 4);
                GTC = GTS + 0x3Cu;
                engine_push(cpu, GTC, 4);
                engine_push(cpu, GTA, 4);
                if (!gather_tree_call(cpu, 0x00553F10u, 0x00554055u)) return 1;
                GTS += 0x2Cu;
                GTP += GTA;
            } else if (GTA != UINT32_MAX) {
                GTD = G32(GTS + 0x58u);
                GTC = G32(GTS + 0x54u);
                engine_push(cpu, GTD, 4);
                GTD = G32(GTS + 0x54u);
                engine_push(cpu, GTC, 4);
                GTC = G32(GTS + 0x4Cu);
                engine_push(cpu, GTD, 4);
                GTC = engine_flags_sub(&cpu->flags, GTC, GTP, 0u, 32u);
                engine_push(cpu, GTC, 4);
                GTD = (uint32_t)(int32_t)(int16_t)GTP;
                GTC = GTB + GTD * 4u;
                engine_push(cpu, GTC, 4);
                GTD = GTS + 0x2Cu;
                engine_push(cpu, GTI, 4);
                engine_push(cpu, GTD, 4);
                GTC = GTT;
                if (!gather_tree_call(cpu, 0x005540C0u, 0x00554089u)) return 1;
                GTS += 0x1Cu;
                GTP += GTA;
            }
        }
        GTA = G32(GTS + 0x5Cu);
        GTC = G32(GTS + 0x10u);
        GTA++;
        S32(GTS + 0x5Cu, GTA);
        GTA = G32(GTS + 0x14u);
        GTC += 4u;
        GTA--;
        S32(GTS + 0x10u, GTC);
        S32(GTS + 0x14u, GTA);
    } while (GTA);

gather_tree_return:
    GTT = engine_pop(cpu, 4);
    GTI = engine_pop(cpu, 4);
    GT_LO16(GTA, GTP);
    GTP = engine_pop(cpu, 4);
    GTB = engine_pop(cpu, 4);
    GTS = engine_flags_add(&cpu->flags, GTS, 0x20u, 0u, 32u);
    cpu->pc = engine_pop(cpu, 4);
    return 1;
}
#undef GTA
#undef GTC
#undef GTD
#undef GTB
#undef GTS
#undef GTP
#undef GTI
#undef GTT
#undef GT_LO16
#endif
