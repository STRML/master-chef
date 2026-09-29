/* Native versions of hot engine functions that only compute.
 *
 * Each of these is a leaf the bearing passes call thousands of times: a
 * matrix product, two bounding-box tests, the BSP bounds decode, the
 * visible-surface list, and the two BSP material walks whose only callback
 * is a bare `ret`. None of them calls anything or touches a global it does
 * not name, so the whole effect of a call is the memory it writes and the
 * registers, flags and x87 state it leaves. The versions here leave exactly
 * that: the same output bytes, the same dead stack frame (saved registers,
 * locals, the argument slots the original reuses), the same eax..edi, the
 * same flags from the last flag-setting instruction, and the same x87 status
 * word and the empty register contents that FLDENV can expose. The
 * arithmetic is the translation's: every x87 value is a binary64
 * holding a float, each operation is the same binary64 operation in the same
 * operand order, and each store rounds to float, so the results are bit for
 * bit the translated code's in round-to-nearest (engine_cpu.h keeps
 * FP_CONTRACT off for this file too, so nothing is fused).
 *
 * A native version keeps its inputs in registers and writes its outputs and
 * its stack frame at the end. That is only the same as the original when
 * nothing it writes is also something it reads, so each checks that its
 * input ranges miss its output ranges and its frame, and otherwise returns 0
 * and lets the translated function run. The same happens when the guest is
 * not rounding to nearest, when the x87 stack has too few free registers for
 * the original's pushes (it would stop with an overflow), or when the
 * direction flag is set for a string copy. tests/test_native_leaves.c runs
 * every one against the translated function on random and edge-case inputs
 * and compares the whole result.
 *
 * HALO_NATIVE_LEAVES=0 turns them all off, for comparing captures. */
#ifndef HALO_NATIVE_LEAVES_H
#define HALO_NATIVE_LEAVES_H
#include "host.h"
#include "engine_flags.h"
/* Like the translated chunks, this needs IEEE binary64 as written:
 * -ffast-math, or -ffp-contract=fast (which ignores the pragma), would fuse
 * or reorder it. */
#ifdef __FAST_MATH__
#error "native_leaves.h must be compiled without -ffast-math"
#endif

static inline double leaf_f32(uint32_t a) { float v; memcpy(&v, GPTR(a), 4); return (double)v; }
static inline void leaf_store_f32(uint32_t a, double v) { float f = (float)v; memcpy(GPTR(a), &f, 4); }
/* A range the original addresses without wrapping past 4 GiB. */
static inline int leaf_span(uint32_t a, uint64_t n) { return (uint64_t)a + n <= UINT64_C(0x100000000); }
static inline int leaf_overlap(uint32_t a, uint64_t n, uint32_t b, uint64_t m) {
    return n && m && (uint64_t)a < (uint64_t)b + m && (uint64_t)b < (uint64_t)a + n;
}
static inline void leaf_return(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4];
    cpu->pc = G32(e);
    cpu->gpr[4] = e + 4u;
}
/* x87 compare result as engine_fp_compare encodes it: C0 0x01, C2 0x04, C3 0x40. */
static inline uint32_t leaf_fcom_bits(double a, double b) {
    return a < b ? 0x01u : a == b ? 0x40u : a > b ? 0u : 0x45u;   /* unordered last */
}
static inline void leaf_fcom_status(EngineCPU *cpu, uint32_t bits) {
    cpu->fp_status = (uint16_t)((cpu->fp_status & ~0x4500u) | (bits << 8));
}
/* The x87 helpers preserve the old shifting stack's logical empty slots:
 * after balanced temporary pushes/pops reaching `depth`, the last `depth`
 * slots duplicate old ST(7-depth). Keep that residue exactly, without
 * changing TOP, tags, or any live register. */
static inline void leaf_fp_temporary_residue(EngineCPU *cpu, unsigned depth) {
    const double tail = cpu->fp_reg[engine_fp_physical(cpu, 7u - depth)];
    for (unsigned i = 8u - depth; i < 8u; i++)
        cpu->fp_reg[engine_fp_physical(cpu, i)] = tail;
}

/* 004CC0D0, reached through the pointer at 0x696664: out = a * b for Halo's
 * 52-byte real_matrix4x3 (scale, forward, left, up, position). cdecl
 * (a, b, out). Every model draw composes each node with it, for every
 * bearing and both eyes, and the game tick composes every object's nodes. If
 * out is a or b the original first copies that operand into its frame. */
static int leaf_matrix4x3_multiply(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4];
    if (!engine_fp_nearest(cpu) || (cpu->fp_valid & 0xC0u) || (cpu->flags & 0x400u)) return 0;
    if (e < 0x3Cu || !leaf_span(e, 0x10)) return 0;
    uint32_t a = G32(e + 4u), b = G32(e + 8u), out = G32(e + 12u), frame = e - 0x3Cu;
    if (!leaf_span(a, 52) || !leaf_span(b, 52) || !leaf_span(out, 52)) return 0;
    /* The frame is the saved edi and esi, the 52-byte copy, the return
     * address and the three arguments. */
    if (leaf_overlap(out, 52, frame, 0x4C) || leaf_overlap(a, 52, frame, 0x3C) || leaf_overlap(b, 52, frame, 0x3C) ||
        (a != out && leaf_overlap(a, 52, out, 52)) || (b != out && leaf_overlap(b, 52, out, 52))) return 0;
    S32(e - 0x38u, cpu->gpr[6]);
    S32(e - 0x3Cu, cpu->gpr[7]);
    uint32_t pa = a, pb = b;
    if (a == out) { memcpy(GPTR(e - 0x34u), GPTR(a), 52); pa = e - 0x34u; cpu->gpr[1] = 0; }
    if (b == out) { memcpy(GPTR(e - 0x34u), GPTR(b), 52); pb = e - 0x34u; cpu->gpr[1] = 0; }
    double A[13], B[13];
    for (unsigned i = 0; i < 13; i++) { A[i] = leaf_f32(pa + 4u * i); B[i] = leaf_f32(pb + 4u * i); }
    /* Row by row in the original's order of operations and operands. */
    leaf_store_f32(out + 0x04u, A[4] * B[2] + A[1] * B[1] + B[3] * A[7]);
    leaf_store_f32(out + 0x08u, A[8] * B[3] + A[2] * B[1] + A[5] * B[2]);
    leaf_store_f32(out + 0x0Cu, A[9] * B[3] + A[3] * B[1] + A[6] * B[2]);
    leaf_store_f32(out + 0x10u, A[1] * B[4] + B[6] * A[7] + A[4] * B[5]);
    leaf_store_f32(out + 0x14u, A[8] * B[6] + A[5] * B[5] + A[2] * B[4]);
    leaf_store_f32(out + 0x18u, A[9] * B[6] + A[6] * B[5] + A[3] * B[4]);
    leaf_store_f32(out + 0x1Cu, B[9] * A[7] + A[1] * B[7] + A[4] * B[8]);
    leaf_store_f32(out + 0x20u, A[2] * B[7] + B[9] * A[8] + B[8] * A[5]);
    leaf_store_f32(out + 0x24u, A[3] * B[7] + B[9] * A[9] + B[8] * A[6]);
    leaf_store_f32(out + 0x28u, (A[1] * B[10] + A[4] * B[11] + B[12] * A[7]) * A[0] + A[10]);
    leaf_store_f32(out + 0x2Cu, (A[8] * B[12] + B[11] * A[5] + B[10] * A[2]) * A[0] + A[11]);
    leaf_store_f32(out + 0x30u, (A[9] * B[12] + B[11] * A[6] + B[10] * A[3]) * A[0] + A[12]);
    leaf_store_f32(out, A[0] * B[0]);
    leaf_fp_temporary_residue(cpu, 2);
    cpu->gpr[0] = pa;
    cpu->gpr[2] = out;
    (void)engine_flags_add(&cpu->flags, e - 0x34u, 0x34u, 0u, 32u);   /* add esp, 0x34 */
    leaf_return(cpu);
    return 1;
}

/* 00553380: one BSP node's bounds from its parent's. ecx = parent bounds
 * (x0,x1,y0,y1,z0,z1), edx = six bytes, esi = out, returns eax = esi. Byte i
 * places out[i] at byte/254 of the way across its axis (the constant at
 * 0x672AD4); 0xFF means the axis maximum. The light and shadow gathers decode
 * one of these for every BSP node they visit. */
static int leaf_bsp_node_bounds(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4], src = cpu->gpr[1], bytes = cpu->gpr[2], out = cpu->gpr[6];
    if (!engine_fp_nearest(cpu) || (cpu->fp_valid & 0xE0u)) return 0;
    if (e < 4u || !leaf_span(e, 4) || !leaf_span(src, 24) || !leaf_span(bytes, 6) || !leaf_span(out, 24)) return 0;
    const uint32_t k_address = 0x00672AD4u;
    if (leaf_overlap(out, 24, src, 24) || leaf_overlap(out, 24, bytes, 6) || leaf_overlap(out, 24, k_address, 4) ||
        leaf_overlap(out, 24, e - 4u, 8) || leaf_overlap(e - 4u, 4, src, 24) || leaf_overlap(e - 4u, 4, bytes, 6) ||
        leaf_overlap(e - 4u, 4, k_address, 4)) return 0;
    /* push ecx makes the slot the fild operand goes through; pop ecx takes back
     * whatever was last put there. */
    uint32_t slot = cpu->gpr[1], last = 0;
    unsigned fp_depth = 2;
    double r[6];
    for (unsigned i = 0; i < 6; i++) {
        uint32_t axis = src + 8u * (i >> 1);
        double lo = leaf_f32(axis), hi = leaf_f32(axis + 4u);
        last = G8(bytes + i);
        if (last == 0xFFu) { r[i] = hi; continue; }
        slot = last;
        fp_depth = 3;
        double n = (double)(int32_t)slot, d = hi - lo, s = n * leaf_f32(k_address), p = d * s;
        r[i] = lo + p;
    }
    for (unsigned i = 0; i < 6; i++) leaf_store_f32(out + 4u * i, r[i]);
    S32(e - 4u, slot);
    leaf_fp_temporary_residue(cpu, fp_depth);
    cpu->gpr[0] = out;
    cpu->gpr[1] = slot;
    cpu->gpr[2] = (cpu->gpr[2] & 0xFFFFFF00u) | last;
    (void)engine_flags_sub(&cpu->flags, last, 0xFFu, 0u, 8u);   /* cmp dl, 0xff */
    leaf_return(cpu);
    return 1;
}

/* 00554260: bounds (eax, x0,x1,y0,y1,z0,z1) against di planes of 16 bytes
 * (normal, distance) at ebx. Returns 0 when all eight corners are behind one
 * plane, 2 when no corner is behind any, else 1. Called for every BSP node and
 * cluster the light and shadow gathers visit. The original evaluates the eight
 * corners with shared partial sums, three of them stored to its frame as
 * floats; that rounding is part of the result, so it is kept. */
static int leaf_bounds_planes(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4], bounds = cpu->gpr[0], planes = cpu->gpr[3];
    int32_t n = (int16_t)cpu->gpr[7];
    const uint32_t zero_address = 0x00672AC0u;
    if (!engine_fp_nearest(cpu) || (cpu->fp_valid & 0xF0u)) return 0;
    if (e < 0x38u || !leaf_span(e, 4) || !leaf_span(bounds, 24)) return 0;
    uint64_t plane_bytes = n > 0 ? 16u * (uint64_t)n : 0u;
    uint32_t t = e - 0x38u;   /* the frame: saved esi, then 0x34 bytes of locals */
    if ((n > 0 && !leaf_span(planes, plane_bytes)) || leaf_overlap(bounds, 24, t, 0x38) ||
        leaf_overlap(planes, plane_bytes, t, 0x38) || leaf_overlap(zero_address, 4, t, 0x38)) return 0;
    uint32_t bw[6];
    for (unsigned i = 0; i < 6; i++) bw[i] = G32(bounds + 4u * i);
    double b[6];
    for (unsigned i = 0; i < 6; i++) { float f; memcpy(&f, &bw[i], 4); b[i] = f; }
    double z = leaf_f32(zero_address);
    uint32_t mask = 0, any = 0, bits = 0, done = 0, result = 2, pw[4] = {0};
    float yc2 = 0, xc0 = 0, zc4 = 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t p = planes + 16u * (uint32_t)i;
        for (unsigned j = 0; j < 4; j++) pw[j] = G32(p + 4u * j);
        double px = leaf_f32(p), py = leaf_f32(p + 4u), pz = leaf_f32(p + 8u), d = leaf_f32(p + 12u);
        zc4 = (float)(b[4] * pz);
        yc2 = (float)(b[2] * py);
        double x0 = b[0] * px;
        xc0 = (float)x0;
        double Y = yc2, Z = zc4, X = xc0;
        double b1px = b[1] * px, b3py = b[3] * py, b5pz = b[5] * pz;
        double t34 = Z + b3py, u = b3py + b5pz;
        double c[8] = {
            x0 + Y + Z - d,
            Y + b1px + Z - d,
            X + t34 - d,
            t34 + b1px - d,
            X + b5pz + Y - d,
            b5pz + b1px + Y - d,
            X + u - d,
            u + b1px - d,
        };
        /* test ah,5 / jp skips the bit unless C0 alone is set: corner < 0. */
        mask = 0;
        for (unsigned k = 0; k < 8; k++) mask |= (uint32_t)(c[k] < z) << k;
        bits = leaf_fcom_bits(c[7], z);
        if (mask == 0xFFu) { result = 0; break; }
        any |= mask;
        done++;
    }
    if (n > 0 && result) result = any ? 1u : 2u;
    S32(t, cpu->gpr[6]);
    for (unsigned i = 0; i < 6; i++) S32(t + 0x20u + 4u * i, bw[i]);
    if (n > 0) {
        memcpy(GPTR(t + 4u), &yc2, 4);
        memcpy(GPTR(t + 8u), &xc0, 4);
        memcpy(GPTR(t + 0xCu), &zc4, 4);
        for (unsigned j = 0; j < 4; j++) S32(t + 0x10u + 4u * j, pw[j]);
        leaf_fcom_status(cpu, bits);
        leaf_fp_temporary_residue(cpu, 4);
    }
    cpu->gpr[0] = result;
    cpu->gpr[1] = n > 0 ? mask : bw[4];
    cpu->gpr[2] = done;
    (void)engine_flags_add(&cpu->flags, e - 0x34u, 0x34u, 0u, 32u);   /* add esp, 0x34 */
    leaf_return(cpu);
    return 1;
}

/* 005541B0: box ecx (x0,x1,y0,y1,z0,z1) against box edx: 0 when they are
 * apart on some axis, 2 when edx lies inside ecx, else 1. Twelve compares, no
 * stores. */
static int leaf_bounds_overlap(EngineCPU *cpu) {
    uint32_t a = cpu->gpr[1], b = cpu->gpr[2];
    if ((cpu->fp_valid & 0x80u) || !leaf_span(a, 24) || !leaf_span(b, 24) || !leaf_span(cpu->gpr[4], 4)) return 0;
    double A[6], B[6];
    for (unsigned i = 0; i < 6; i++) { A[i] = leaf_f32(a + 4u * i); B[i] = leaf_f32(b + 4u * i); }
    /* test ah,5 / jnp jumps when C0 alone is set (x < y); test ah,0x41 / je
     * when neither C0 nor C3 is (x > y). The first six compares mean apart,
     * the last six mean not inside. */
    uint32_t bits = 0, test = 0, result;
#define LEAF_BELOW(x, y) (bits = leaf_fcom_bits(x, y), test = bits & 0x05u, test == 0x01u)
#define LEAF_ABOVE(x, y) (bits = leaf_fcom_bits(x, y), test = bits & 0x41u, test == 0u)
    if (LEAF_BELOW(A[1], B[0]) || LEAF_ABOVE(A[0], B[1]) || LEAF_BELOW(A[3], B[2]) ||
        LEAF_ABOVE(A[2], B[3]) || LEAF_BELOW(A[5], B[4]) || LEAF_ABOVE(A[4], B[5])) result = 0;
    else if (LEAF_BELOW(B[0], A[0]) || LEAF_ABOVE(B[1], A[1]) || LEAF_BELOW(B[2], A[2]) ||
             LEAF_ABOVE(B[3], A[3]) || LEAF_BELOW(B[4], A[4]) || LEAF_ABOVE(B[5], A[5])) result = 1;
    else result = 2;
#undef LEAF_BELOW
#undef LEAF_ABOVE
    leaf_fcom_status(cpu, bits);
    leaf_fp_temporary_residue(cpu, 1);
    if (result) (void)engine_flags_logic(&cpu->flags, test, 8u);   /* the last test ah */
    else (void)engine_flags_logic(&cpu->flags, 0u, 32u);             /* xor eax, eax */
    cpu->gpr[0] = result;
    leaf_return(cpu);
    return 1;
}

/* 00552C20: cdecl (list, bitset, indices). For each surface of the BSP at
 * [0x746F9C] whose bit is set, append its index to list and its three vertex
 * indices to indices. Once per bearing over every surface of the level, one
 * bit at a time in the original. It advances the bitset pointer in its own
 * argument slot and keeps its bit counter in the slot ecx was pushed to, so
 * pop ecx returns the last counter. */
static int leaf_surface_list(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4];
    if (e < 0x14u || !leaf_span(e, 0x10)) return 0;
    uint32_t list = G32(e + 4u), bitset = G32(e + 8u), indices = G32(e + 12u);
    uint32_t bsp = G32(0x00746F9Cu);
    if (!leaf_span(bsp, 0x100)) return 0;
    int32_t count = (int32_t)G32(bsp + 0xF8u);
    uint32_t triangles = G32(bsp + 0xFCu);
    uint32_t ecx = cpu->gpr[1], edx = 0;
    if (count > 0) {
        uint32_t words = ((uint32_t)count + 31u) >> 5, found = 0;
        if (count > 0x100000 || !leaf_span(bitset, 4u * (uint64_t)words) || !leaf_span(triangles, 6u * (uint64_t)count)) return 0;
        for (uint32_t w = 0; w < words; w++) {
            uint32_t v = G32(bitset + 4u * w), left = (uint32_t)count - 32u * w;
            if (left < 32u) v &= (1u << left) - 1u;
            found += (uint32_t)__builtin_popcount(v);
        }
        /* The written ranges must miss everything read: the BSP header, the
         * bitset, the triangles, the frame and the arguments. movsx di limits
         * the list to 0x7FFF entries. */
        uint64_t list_bytes = 4u * (uint64_t)found, index_bytes = 6u * (uint64_t)found;
        if (found > 0x7FFFu || !leaf_span(list, list_bytes) || !leaf_span(indices, index_bytes)) return 0;
        const uint32_t read_at[4] = {0x00746F9Cu, bsp + 0xF8u, bitset, triangles};
        const uint64_t read_bytes[4] = {4u, 8u, 4u * (uint64_t)words, 6u * (uint64_t)count};
        for (unsigned r = 0; r < 4; r++)
            if (leaf_overlap(list, list_bytes, read_at[r], read_bytes[r]) || leaf_overlap(indices, index_bytes, read_at[r], read_bytes[r]) ||
                leaf_overlap(e - 0x14u, 0x14, read_at[r], read_bytes[r]) || leaf_overlap(e + 8u, 4, read_at[r], read_bytes[r])) return 0;
        if (leaf_overlap(list, list_bytes, indices, index_bytes) || leaf_overlap(list, list_bytes, e - 0x14u, 0x24) ||
            leaf_overlap(indices, index_bytes, e - 0x14u, 0x24)) return 0;
        uint32_t n = 0, word = bitset;
        do {
            uint32_t v = G32(word);
            if (v) {
                uint32_t bits = (uint32_t)count - edx < 32u ? (uint32_t)count - edx : 32u;
                if (bits < 32u) v &= (1u << bits) - 1u;
                while (v) {
                    uint32_t s = edx + (uint32_t)__builtin_ctz(v);
                    S32(list + 4u * n, s);
                    memcpy(GPTR(indices + 6u * n), GPTR(triangles + 6u * s), 6);
                    n++;
                    v &= v - 1u;
                }
                edx += bits;
                ecx = bits;
            } else edx += 32u;
            word += 4u;
        } while ((int32_t)edx < count);
        S32(e - 0x10u, cpu->gpr[3]);
        S32(e - 0x14u, cpu->gpr[6]);
        S32(e + 8u, word);
        (void)engine_flags_sub(&cpu->flags, edx, (uint32_t)count, 0u, 32u);   /* cmp edx, ecx */
    } else (void)engine_flags_logic(&cpu->flags, (uint32_t)count, 32u);         /* test ecx, ecx */
    S32(e - 4u, ecx);
    S32(e - 8u, cpu->gpr[5]);
    S32(e - 0xCu, cpu->gpr[7]);
    cpu->gpr[0] = bsp;
    cpu->gpr[1] = ecx;
    cpu->gpr[2] = edx;
    leaf_return(cpu);
    return 1;
}

/* 00552DE0 with no callbacks but the bare `ret` at 0x44AD80. The walk is
 * usercall (eax = surface count, ecx = sorted surface list) and cdecl
 * (buffer, per lightmap, per opaque material, lightmap end, per transparent
 * material). For each lightmap and each of its materials it finds the run of
 * listed surfaces and calls the matching callback; it writes nothing but its
 * frame. 0050BFB0 walks the BSP nine times per bearing, and the two walks at
 * 0050C523 and 0050C544 pass only 0x44AD80, so all they leave is that frame
 * and eax, ecx and edx. This runs the same walk without the callbacks and
 * leaves exactly those. Any other set of callbacks runs the original. */
static int leaf_bsp_walk_dead(EngineCPU *cpu) {
    uint32_t e = cpu->gpr[4];
    if (e < 0x48u || !leaf_span(e, 0x18)) return 0;
    uint32_t buffer = G32(e + 4u), arg2 = G32(e + 12u);
    if (G32(e + 8u) || G32(e + 16u) || G32(e + 20u) || (arg2 && arg2 != 0x0044AD80u)) return 0;
    /* Every read is checked against the frame [e-0x48, e); a hit hands the
     * call to the original before anything is written. */
    const uint32_t lo = e - 0x48u;
#define WALK_READ(bits, a) ({ uint32_t a_ = (a); if (!leaf_span(a_, (bits) / 8) || leaf_overlap(a_, (bits) / 8, lo, 0x48)) return 0; G##bits(a_); })
    uint32_t ebx = cpu->gpr[1] + 4u * (uint32_t)(int32_t)(int16_t)cpu->gpr[0];
    const uint32_t list = cpu->gpr[1], end = ebx;
    uint32_t bsp = WALK_READ(32, 0x00746F9Cu);
    uint32_t lightmaps = WALK_READ(32, bsp + 0x104u);
    uint32_t eax = 0, ecx = lightmaps, edx = cpu->gpr[2];
    uint32_t run_total = 0, lightmap_index = 0, lightmap = 0, bitmap = 0, material_index = 0;
    int wrote_lightmap = 0, wrote_bitmap = 0, wrote_material = 0, called = 0;
    uint32_t call[7] = {0};
    ebx = list;
    if ((int32_t)lightmaps > 0) {
        for (;;) {
            if (ebx >= end) break;                                   /* 00552E20: list exhausted */
            uint32_t lm = WALK_READ(32, bsp + 0x108u) + ((uint32_t)(int32_t)(int16_t)eax << 5);
            uint32_t materials = WALK_READ(32, lm + 0x18u);
            uint32_t last = (WALK_READ(32, lm + 0x14u) << 8) + materials - 0x100u;
            uint32_t lm_end = WALK_READ(32, last + 0x18u) + WALK_READ(32, last + 0x14u);
            lightmap = lm; wrote_lightmap = 1;
            if ((int32_t)WALK_READ(32, ebx) < (int32_t)lm_end) {
                uint32_t bitmap_tag = WALK_READ(32, bsp + 0xCu), bm = 0;
                if (bitmap_tag != 0xFFFFFFFFu) {
                    uint32_t tags = WALK_READ(32, 0x0087BC14u);
                    int32_t which = (int16_t)WALK_READ(16, lm);
                    uint32_t data = WALK_READ(32, ((bitmap_tag & 0xFFFFu) << 5) + tags + 0x14u);
                    if (data && which >= 0 && which < (int32_t)WALK_READ(32, data + 0x60u))
                        bm = (uint32_t)which * 48u + WALK_READ(32, data + 0x64u);
                }
                bitmap = bm; wrote_bitmap = 1;
                edx = 0; material_index = 0; wrote_material = 1;
                if ((int32_t)WALK_READ(32, lm + 0x14u) > 0) {
                    for (;;) {
                        if (ebx >= end) break;                           /* 00552ED0 */
                        uint32_t mat = ((uint32_t)(int32_t)(int16_t)edx << 8) + WALK_READ(32, lm + 0x18u);
                        uint32_t mat_end = WALK_READ(32, mat + 0x18u) + WALK_READ(32, mat + 0x14u);
                        if ((int32_t)WALK_READ(32, ebx) < (int32_t)mat_end) {
                            uint32_t shader = WALK_READ(32, ((WALK_READ(32, mat + 0xCu) & 0xFFFFu) << 5) + WALK_READ(32, 0x0087BC14u) + 0x14u);
                            uint32_t start = ebx;
                            do ebx += 4u; while (ebx < end && (int32_t)WALK_READ(32, ebx) < (int32_t)mat_end);
                            uint32_t run = (uint32_t)((int32_t)(ebx - start) >> 2);
                            int16_t breakable = (int16_t)WALK_READ(16, mat + 0xACu);
                            int live = 1;
                            if (breakable != -1) {
                                int32_t which = breakable;
                                uint32_t word = (uint32_t)(which >> 5) + (uint32_t)(int32_t)(int16_t)WALK_READ(16, 0x0069E8D8u) * 8u;
                                live = (WALK_READ(32, WALK_READ(32, 0x006B8D78u) + word * 4u + 1u) & (1u << (which & 31))) != 0;
                            }
                            if (live) {
                                int32_t type = (int16_t)WALK_READ(16, shader + 0x24u);
                                int transparent = type == 1 || (type > 4 && type <= 0xB);
                                if (!transparent && arg2) {
                                    /* The frame of the last opaque callback, pushed right to left. */
                                    call[0] = mat + 0xB0u;
                                    call[1] = (uint32_t)(int32_t)(int16_t)run;
                                    call[2] = run_total;
                                    call[3] = buffer;
                                    call[4] = (uint32_t)(int32_t)(int16_t)WALK_READ(16, mat + 0x10u);
                                    call[5] = shader;
                                    call[6] = 0x00552FA0u;
                                    called = 1;
                                }
                            }
                            run_total += (uint32_t)(int32_t)(int16_t)run;
                        }
                        edx++;                                               /* 0055301C */
                        material_index = edx;
                        if (!((int32_t)(int16_t)edx < (int32_t)WALK_READ(32, lm + 0x14u))) break;
                    }
                }
            }
            eax = lightmap_index + 1u;                                   /* 00553039 */
            lightmap_index = eax;
            ecx = (uint32_t)(int32_t)(int16_t)eax;
            edx = WALK_READ(32, bsp + 0x104u);
            if (!((int32_t)ecx < (int32_t)edx)) break;
        }
    }
#undef WALK_READ
    S32(e - 0x20u, cpu->gpr[3]);
    S32(e - 0x24u, cpu->gpr[6]);
    S32(e - 0x18u, end);
    S32(e - 4u, bsp);
    S32(e - 0x1Cu, run_total);
    S32(e - 8u, lightmap_index);
    if ((int32_t)lightmaps > 0) {
        S32(e - 0x28u, cpu->gpr[5]);
        S32(e - 0x2Cu, cpu->gpr[7]);
        if (wrote_lightmap) S32(e - 0x14u, lightmap);
        if (wrote_bitmap) S32(e - 0x10u, bitmap);
        if (wrote_material) S32(e - 0xCu, material_index);
        if (called) for (unsigned i = 0; i < 7; i++) S32(e - 0x30u - 4u * i, call[i]);
    }
    cpu->gpr[0] = eax;
    cpu->gpr[1] = ecx;
    cpu->gpr[2] = edx;
    (void)engine_flags_add(&cpu->flags, e - 0x1Cu, 0x1Cu, 0u, 32u);   /* add esp, 0x1c */
    leaf_return(cpu);
    return 1;
}

static int host_native_leaves_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *o = HOST_ENV("HALO_NATIVE_LEAVES"); enabled = !(o && o[0] == '0'); }
    return enabled;
}
/* Called first in engine_dispatch_override. 00552DE0 is handled after the
 * HALO_A10_TRACE boundary instead, so the trace still sees every walk. */
static int host_native_leaf_dispatch(EngineCPU *cpu, uint32_t address) {
    if (address == 0x004CC0D0u) return host_native_leaves_enabled() && leaf_matrix4x3_multiply(cpu);
    if (address == 0x00554260u) return host_native_leaves_enabled() && leaf_bounds_planes(cpu);
    if (address == 0x005541B0u) return host_native_leaves_enabled() && leaf_bounds_overlap(cpu);
    if (address == 0x00553380u) return host_native_leaves_enabled() && leaf_bsp_node_bounds(cpu);
    if (address == 0x00552C20u) return host_native_leaves_enabled() && leaf_surface_list(cpu);
    return 0;
}
#endif
