/* Native 00553920: mark the triangles of every visible subcluster.
 *
 * 005537C0 floods the portals from the camera cluster and leaves one
 * 0x1A0-byte record per visible cluster at 007C3390, (int16)[007D0390] of
 * them, each holding the cluster index and a frustum narrowed to the portals
 * it was seen through (at +0x14). 00553920 (cdecl, one argument: the
 * structure BSP) then tests every subcluster of every one of those clusters
 * against that frustum with 0050D5B0 and, for each one that survives, sets
 * its triangles' bits in 007D0394 and counts the new ones in [00850394], up
 * to 0x4000. In PVS mode ([00724A45], the structures_use_pvs_for_vs global)
 * or with the camera outside the BSP ([007C3348] == -1) it tests against
 * the view frustum at 007C3168 instead.
 *
 * Every bearing pass runs this for each view it renders. 0050D5B0 is about
 * 670 translated instructions, 400 of them emulated x87 operations, and it
 * runs for each of a few hundred subclusters a view; before the x87 fast
 * path it was the hottest guest function in the b30 profiles, and it is
 * still most of 00553920's time. Its only other caller (00458E49) keeps the
 * translated routine.
 *
 * 0050D5B0 with the flag 00553920 passes (zero):
 *   - Reject (return 0) when the frustum's world bounds miss the box:
 *     F+0x12C < x0, F+0x134 < y0, F+0x13C < z0, F+0x128 > x1, F+0x130 > y1
 *     or F+0x138 > z1. The FCOMP/FNSTSW tests take only ordered results,
 *     so a NaN on either side never rejects.
 *   - Otherwise copy the eight corners to its stack frame and give each an
 *     outcode against the four side planes at F+0x78 (n.x, n.y, n.z, d),
 *     one bit per plane whose value is above [00672AC0]. It evaluates each
 *     plane in its own order, and that order is kept here because the sums
 *     round: ((x*n.x + y*n.y) + n.z*z) - d for the first plane,
 *     ((n.x*x + n.z*z) + y*n.y) - d for the second and
 *     ((n.z*z + y*n.y) + n.x*x) - d for the other two. A product is the
 *     same IEEE operation on the same two floats whichever corner asks for
 *     it, so each is formed once per box; the sums are formed per corner.
 *   - Return 2 when no corner is outside any plane, 0 when all eight are
 *     outside one plane, and 1 otherwise.
 *
 * The emulated x87 evaluates everything in binary64 whatever the precision
 * control, so this is plain double arithmetic with contraction off. In any
 * rounding mode but nearest (Halo runs at 0x027F) the translated routine
 * runs instead, as it does when the x87 stack is too deep for 0050D5B0's
 * two extra registers; there it would stop the engine just the same.
 *
 * This follows the original's registers and writes its guest memory in the
 * same order: the locals and saved registers of 00553920's frame, the
 * argument and return address of each call, 0050D5B0's frame whenever a
 * box gets past the bounds test (saved registers, loop counter, corner
 * copies), the bitset words and the count. It reads back every stack slot
 * the original reads back, and rereads the BSP, the records, the frustums
 * and the counts after each write the original makes in between, so even a
 * triangle id so far out of range that its bitset word lands in one of
 * those comes out the same. (The one thing not followed is a frustum that
 * lies in 0050D5B0's own frame, where the original would read its loop
 * counter as a plane; that takes a frustum pointer bent into the stack.)
 * What the caller sees afterwards is the original's too: the registers as
 * it leaves them (the saved ones popped back off the frame), the flags of
 * its final ADD ESP, the condition bits of the last FCOMP and the stale x87
 * registers its pushes and pops leave behind. tests/test_visible_surfaces.c
 * runs the translated routine beside this one and compares all of it byte
 * for byte. */
#ifndef HALO_VISIBLE_SURFACES_H
#define HALO_VISIBLE_SURFACES_H
#include "host.h"
#include "engine_flags.h"

#define VS_RECORDS      0x007C3390u   /* 0x1A0-byte visible-cluster records */
#define VS_RECORD_COUNT 0x007D0390u   /* int16 */
#define VS_BITSET       0x007D0394u   /* one bit per BSP triangle */
#define VS_MARKED       0x00850394u   /* int16 count of bits set */
#define VS_VIEW_FRUSTUM 0x007C3168u
#define VS_PVS_MODE     0x00724A45u   /* structures_use_pvs_for_vs */
#define VS_CAMERA_CLUSTER 0x007C3348u
#define VS_PLANE_LIMIT  0x00672AC0u   /* the float 0050D5B0 compares plane values with */
#define VS_CALL_RETURN  0x005539D6u   /* 00553920's return address into itself */
#define VS_TRIANGLE_CAP 0x4000

static inline float vs_float(uint32_t address) { float v; memcpy(&v, GPTR(address), 4); return v; }
static inline uint32_t vs_sx16(uint32_t value) { return (uint32_t)(int32_t)(int16_t)value; }
/* FCOMP's C3/C2/C0 exactly as engine_fp_compare sets them. */
static inline uint16_t vs_compare_bits(double a, double b) {
    return (uint16_t)((isnan(a) || isnan(b)) ? 0x4500u : a < b ? 0x0100u : a == b ? 0x4000u : 0u);
}
/* FLD dword then FST dword: binary32 to binary64 and back, which only
 * changes a signalling NaN (the conversion quiets it). */
static inline float vs_float_round_trip(float value) {
    volatile double wide = value;
    return (float)wide;
}

/* 0050D5B0(ecx = frustum, edi = box, flag 0) called with its return address
 * at `ret` and EBX, EBP and ESI as given. Returns 0, 1 or 2 as it does, and
 * past the bounds test writes the frame it leaves below `ret`: saved ESI,
 * EBP and EBX, the corner loop's counter (zero at the end) and the eight
 * corners, x fastest. Raises *depth to the x87 depth it reached and leaves
 * the last FCOMP's condition bits in *compare. */
static inline int vs_box_test(uint32_t ret, uint32_t frustum, uint32_t box, uint32_t ebx, uint32_t ebp, uint32_t esi,
                              unsigned *depth, uint16_t *compare) {
#pragma clang fp contract(off)
    double a, b;
    a = vs_float(frustum + 0x12Cu); b = vs_float(box + 0x00u); if (a < b) goto reject;
    a = vs_float(frustum + 0x134u); b = vs_float(box + 0x08u); if (a < b) goto reject;
    a = vs_float(frustum + 0x13Cu); b = vs_float(box + 0x10u); if (a < b) goto reject;
    a = vs_float(frustum + 0x128u); b = vs_float(box + 0x04u); if (a > b) goto reject;
    a = vs_float(frustum + 0x130u); b = vs_float(box + 0x0Cu); if (a > b) goto reject;
    a = vs_float(frustum + 0x138u); b = vs_float(box + 0x14u); if (a > b) goto reject;
    {
        float stored[6];            /* x0 x1 y0 y1 z0 z1 as the corner copies hold them */
        for (unsigned i = 0; i < 6; i++) stored[i] = vs_float_round_trip(vs_float(box + 4u * i));
        S32(ret - 0x70u, esi); S32(ret - 0x6Cu, ebp); S32(ret - 0x68u, ebx); S32(ret - 0x64u, 0);
        for (unsigned c = 0; c < 8; c++) {
            const float corner[3] = { stored[c & 1u], stored[2u + ((c >> 1) & 1u)], stored[4u + (c >> 2)] };
            memcpy(GPTR(ret - 0x60u + 12u * c), corner, sizeof corner);
        }
        /* The corner loop reads the planes and the limit after the copies. */
        const double x[2] = { stored[0], stored[1] }, y[2] = { stored[2], stored[3] }, z[2] = { stored[4], stored[5] };
        double n[16];
        for (unsigned i = 0; i < 16; i++) n[i] = vs_float(frustum + 0x78u + 4u * i);
        const double limit = vs_float(VS_PLANE_LIMIT);
        double x0[2], y0[2], z0[2], x1[2], y1[2], z1[2], x2[2], y2[2], z2[2], x3[2], y3[2], z3[2];
        for (unsigned s = 0; s < 2; s++) {
            x0[s] = x[s] * n[0];  y0[s] = y[s] * n[1];  z0[s] = n[2] * z[s];
            x1[s] = n[4] * x[s];  z1[s] = n[6] * z[s];  y1[s] = y[s] * n[5];
            z2[s] = n[10] * z[s]; y2[s] = y[s] * n[9];  x2[s] = n[8] * x[s];
            z3[s] = n[14] * z[s]; y3[s] = y[s] * n[13]; x3[s] = n[12] * x[s];
        }
        unsigned all = 0x3Fu, any = 0;
        double last = 0;
        for (unsigned c = 0; c < 8; c++) {
            const unsigned i = c & 1u, j = (c >> 1) & 1u, k = c >> 2;
            double p0 = x0[i] + y0[j]; p0 = p0 + z0[k]; p0 = p0 - n[3];
            double p1 = x1[i] + z1[k]; p1 = p1 + y1[j]; p1 = p1 - n[7];
            double p2 = z2[k] + y2[j]; p2 = p2 + x2[i]; p2 = p2 - n[11];
            double p3 = z3[k] + y3[j]; p3 = p3 + x3[i]; p3 = p3 - n[15];
            const unsigned code = (unsigned)(p0 > limit) | (unsigned)(p1 > limit) << 1 |
                                  (unsigned)(p3 > limit) << 2 | (unsigned)(p2 > limit) << 3;
            all &= code; any |= code;
            last = p3;
        }
        *depth = 2;
        *compare = vs_compare_bits(last, limit);
        return !any ? 2 : all ? 0 : 1;
    }
reject:
    if (!*depth) *depth = 1;
    *compare = vs_compare_bits(a, b);
    return 0;
}

/* Runs 00553920 at its entry (esp at the return address). Returns 0, having
 * touched nothing, when the translated routine must run instead. The labels
 * name the original's instructions. */
static int host_mark_visible_surfaces(EngineCPU *cpu) {
    if (!engine_fp_nearest(cpu) || (cpu->fp_valid & 0xC0u)) return 0;
    const uint32_t entry = cpu->gpr[4];
    uint32_t eax = 0, ecx = cpu->gpr[1], edx = cpu->gpr[2], ebx = cpu->gpr[3];
    uint32_t ebp = cpu->gpr[5], esi = cpu->gpr[6], edi = cpu->gpr[7];
    unsigned depth = 0;          /* deepest x87 stack any 0050D5B0 call reached */
    uint16_t compare = 0;
    const int16_t first_count = (int16_t)G16(VS_RECORD_COUNT);                /* 00553925 */
    S32(entry - 0x04u, 0);                                                    /* 0055392C: record index */
    if (first_count > 0) {
        S32(entry - 0x10u, ebx); S32(entry - 0x14u, ebp);                     /* 00553936 */
        S32(entry - 0x18u, esi); S32(entry - 0x1Cu, edi);
        for (;;) {
            if ((int16_t)G16(VS_MARKED) >= VS_TRIANGLE_CAP) break;          /* 00553940 */
            const uint32_t bsp = G32(entry + 4u);
            edx = G32(bsp + 0x138u);
            const uint8_t pvs_mode = G8(VS_PVS_MODE);
            ecx = (bsp & 0xFFFFFF00u) | pvs_mode;
            const uint32_t record = vs_sx16(eax) * 0x1A0u + VS_RECORDS;
            ebx = vs_sx16(G16(record)) * 0x68u + edx;                        /* the cluster */
            S32(entry - 0x0Cu, pvs_mode || G32(VS_CAMERA_CLUSTER) == 0xFFFFFFFFu ? VS_VIEW_FRUSTUM : record + 0x14u);
            const uint32_t first_subclusters = G32(ebx + 0x34u);               /* 00553993 */
            esi = 0;                                                          /* subcluster */
            S32(entry - 0x08u, esi);
            if ((int32_t)first_subclusters > 0) {
                for (;;) {
                    if ((int16_t)G16(VS_MARKED) >= VS_TRIANGLE_CAP) break;  /* 005539B0 */
                    ecx = G32(entry - 0x0Cu);
                    edi = G32(ebx + 0x38u) + 36u * vs_sx16(esi);
                    S32(entry - 0x20u, 0); S32(entry - 0x24u, VS_CALL_RETURN);
                    if (vs_box_test(entry - 0x24u, ecx, edi, ebx, ebp, esi, &depth, &compare)) {
                        const uint32_t triangles = G32(edi + 0x18u);            /* 005539DE */
                        uint32_t list = G32(edi + 0x1Cu);
                        ebp = 0;
                        if ((int32_t)triangles > 0) {
                            for (;;) {
                                const uint32_t triangle = G32(list);            /* 005539F0 */
                                edx = 1u << (triangle & 31u);
                                const uint32_t word = (uint32_t)((int32_t)triangle >> 5) * 4u + VS_BITSET;
                                ecx = G32(word);
                                if (!(ecx & edx)) {
                                    if ((int16_t)G16(VS_MARKED) >= VS_TRIANGLE_CAP) break;   /* 00553A13 */
                                    ecx |= edx;
                                    S32(word, ecx);
                                    S16(VS_MARKED, (uint16_t)(G16(VS_MARKED) + 1u));
                                }
                                const uint32_t count = G32(edi + 0x18u);        /* 00553A29 */
                                list += 4u; ebp++;
                                ecx = vs_sx16(ebp);
                                if (!((int32_t)ecx < (int32_t)count)) break;
                            }
                        }
                        esi = G32(entry - 0x08u);                                 /* 00553A37 */
                    }
                    const uint32_t subclusters = G32(ebx + 0x34u);              /* 00553A3B */
                    esi++;
                    edx = vs_sx16(esi);
                    S32(entry - 0x08u, esi);
                    if (!((int32_t)edx < (int32_t)subclusters)) break;
                }
            }
            eax = G32(entry - 0x04u) + 1u;                                        /* 00553A4E */
            const uint16_t records = G16(VS_RECORD_COUNT);
            S32(entry - 0x04u, eax);
            if (!((int16_t)eax < (int16_t)records)) break;
        }
        edi = G32(entry - 0x1Cu); esi = G32(entry - 0x18u);                     /* 00553A64 */
        ebp = G32(entry - 0x14u); ebx = G32(entry - 0x10u);
    }
    if (depth) {
        /* Balanced temporary pushes/pops duplicate the old logical tail,
         * including empty registers retained for a later FLDENV. The
         * rotating register file keeps those logical slots behind TOP. */
        const double tail = cpu->fp_reg[engine_fp_physical(cpu, 7u - depth)];
        for (unsigned slot = 8u - depth; slot < 8u; slot++)
            cpu->fp_reg[engine_fp_physical(cpu, slot)] = tail;
        cpu->fp_status = (uint16_t)((cpu->fp_status & ~0x4500u) | compare);
    }
    cpu->gpr[0] = eax; cpu->gpr[1] = ecx; cpu->gpr[2] = edx; cpu->gpr[3] = ebx;
    cpu->gpr[5] = ebp; cpu->gpr[6] = esi; cpu->gpr[7] = edi;
    (void)engine_flags_add(&cpu->flags, entry - 0x0Cu, 0x0Cu, 0u, 32u);         /* 00553A68: add esp, 0xc */
    cpu->pc = G32(entry);
    cpu->gpr[4] = entry + 4u;
    return 1;
}
#endif
