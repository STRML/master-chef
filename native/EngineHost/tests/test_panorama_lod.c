/* One object pixel size per frame in every bearing (panorama_lod.h).
 *
 * 1. Against the translated 0050F740 over random and awkward inputs (NaN,
 *    infinities, signed zeros, subnormals, the 0.1 clamp, the FLT_MAX special
 *    case, every radius switch, every rounding mode, non-retail constants):
 *    with the original depth the native routine is bit-identical: every guest
 *    byte, EAX..EDI, EFLAGS, the return and ESP, all eight x87 slots, the tag
 *    bits, TOP, the control and status words. With the frame's depth
 *    (DISTANCE, NEAREST) everything but ST0 and the condition bits that
 *    compared it is still identical, and ST0 equals an independent reference
 *    2 r S / max(|depth|, 0.1). This part needs the generated tree; without it
 *    it prints SKIP and the native original stands in below.
 * 2. A modelled stereo frame, nine passes, many head poses and objects: each
 *    object gets one pixel size in every pass and both eyes, so its LOD, draw
 *    cutoff, shadow, shadow fade, silhouette LOD and lighting cadence agree
 *    wherever it is drawn; the original's disagreements are counted. In the
 *    centre bearing DISTANCE is cos(angle off its axis) of the original, to
 *    within the original's own rounding, and exact on the axis; NEAREST is the
 *    original over each bearing's own sector and never above it elsewhere.
 * 3. The hook inside the real pass loop (panorama_hooks.inc): every pass and
 *    both eyes get the same value, S is the centre view's, the mirror pass,
 *    a foreign camera, a full x87 stack and HALO_PANORAMA_LOD=bearing keep
 *    the original, mono frames are hooked too, and each hooked call leaves the
 *    guest as the original does apart from ST0 and C0/C2/C3.
 * 4. Time per call of each version. */
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 1   /* the release setting */
#endif
#include "../host.h"
#include "../halo_settings.h"
#undef HOST_ENV
#define HOST_ENV(name) getenv(name)
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

uint8_t *engine_flat_base;
uint32_t engine_trace_lo, engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu) { (void)cpu; }
void host_log(const char *format, ...) { (void)format; }
void host_panorama_reset(void) {}
void host_panorama_invalidate(void) {}
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float *pose) { (void)pose; }
void host_panorama_abort(void) {}
void host_panorama_begin(int pass) { (void)pass; }
void host_panorama_end(int pass) { (void)pass; }
void host_panorama_projection(float x, float y, int vx, int vy, int w, int h, uint32_t caller) {
    (void)x; (void)y; (void)vx; (void)vy; (void)w; (void)h; (void)caller;
}
void host_panorama_ui(int active) { (void)active; }
void host_panorama_viewmodel(int active) { (void)active; }
static void ret_to_caller(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }
#define OARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))
uint64_t host_yield_spin_ns;
int host_panorama_all_layers_ready(void) { return 0; }
void host_panorama_set_stereo(int on) { (void)on; }
int host_pass_profile_enabled(void) { return 0; }
void host_pass_profile_add(uint64_t ns) { (void)ns; }
/* These tests keep the unpaced bearing target. */
float host_frame_pacer_budget_target(float fallback) { return fallback; }
#include "../panorama_hooks.inc"

/* HALO_TEST_WITHOUT_GENERATED=1 builds it as a checkout without the tree would. */
#if !HALO_TEST_WITHOUT_GENERATED && \
    __has_include("sub_0050F740.c")
#define HAVE_ORIGINAL 1
#include "engine_registers.h"
#include "sub_0050F740.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags
#else
#define HAVE_ORIGINAL 0
#endif

/* The original: the translated routine when the tree is here, otherwise the
 * native one with the original depth, which part 1 proves identical. */
static void run_original(EngineCPU *cpu) {
#if HAVE_ORIGINAL
    cpu->pc = 0x0050F740u; sub_0050F740(cpu);
#else
    halo_panorama_lod_object_pixels(cpu, NULL);
#endif
}

enum { MEMORY = 0x900000 };
enum { GLOBALS = 0x100000, OBJECT_HEADER = 0x110000, OBJECT_TABLE = 0x120000, OBJECTS = 0x200000,
       STACK = 0x300000, OBJECT_COUNT = 1024 };
static uint32_t object_at(unsigned i) { return OBJECTS + i * 0x200u; }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t next32(void) { return (uint32_t)(next64() >> 32); }
static double unit(void) { return (double)(next64() >> 11) * 0x1.0p-53; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint32_t fbits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static float bitsf(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
static uint64_t dbits(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }
static void setf(uint32_t address, float v) { memcpy(GPTR(address), &v, 4); }

static float awkward(void) {
    static const uint32_t special[] = {
        0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x3DCCCCCDu, 0xBDCCCCCDu, 0x3DCCCCCCu, 0x3DCCCCCEu,
        0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u,
        0x7FC00000u, 0xFFC00000u, 0x7FA00001u, 0x7FC12345u, 0x41F00000u, 0x42340000u, 0x43C80000u, 0x42C80000u };
    return bitsf(special[next32() % (sizeof special / sizeof *special)]);
}
static float random_float(void) {
    uint32_t r = next32() % 16;
    if (r == 0) return awkward();
    if (r == 1) return bitsf(next32());
    double magnitude = pow(10.0, unit() * 8.0 - 4.0);
    return (float)((next32() & 1) ? magnitude : -magnitude);
}
/* A head pose (position, forward, up) as the game camera gives it. */
static void random_pose(float pose[9], float roll) {
    float yaw = (float)(unit() * 2 * M_PI), pitch = (float)((unit() - 0.5) * 1.2);
    for (int k = 0; k < 3; k++) pose[k] = (float)((unit() - 0.5) * 200.0);
    float f[3] = { cosf(yaw)*cosf(pitch), sinf(yaw)*cosf(pitch), sinf(pitch) };
    float level_up[3] = { -cosf(yaw)*sinf(pitch), -sinf(yaw)*sinf(pitch), cosf(pitch) };
    float right[3] = { f[1]*level_up[2]-f[2]*level_up[1], f[2]*level_up[0]-f[0]*level_up[2], f[0]*level_up[1]-f[1]*level_up[0] };
    for (int k = 0; k < 3; k++) { pose[3 + k] = f[k]; pose[6 + k] = level_up[k]*cosf(roll) + right[k]*sinf(roll); }
}

/* The panorama's nine passes, as panorama_hooks.inc schedules them. */
static const HaloPanoramaView passes[] = {
    { 0,                         -1.0471975512f, 0.f,            0 },
    { 2,                          1.0471975512f, 0.f,            0 },
    { HALO_PANORAMA_UP,           0.f,           1.5707963268f,  0 },
    { HALO_PANORAMA_DOWN,         0.f,          -1.5707963268f,  0 },
    { HALO_PANORAMA_REAR_RIGHT,   2.0943951024f, 0.f,            0 },
    { HALO_PANORAMA_REAR,         3.1415926536f, 0.f,            0 },
    { HALO_PANORAMA_REAR_LEFT,   -2.0943951024f, 0.f,            0 },
    { HALO_PANORAMA_CENTRE_RIGHT, 0.f,           0.f,            1 },
    { HALO_PANORAMA_CENTRE_LEFT,  0.f,           0.f,            0 },
};
enum { PASSES = sizeof passes / sizeof *passes, CENTRE_RIGHT_PASS = PASSES - 2, CENTRE_LEFT_PASS = PASSES - 1 };

/* ---------------------------------------------------------------- part 1 */
static uint8_t *memory_a, *memory_b;
static void put32(uint32_t address, uint32_t v) { memcpy(memory_a + address, &v, 4); memcpy(memory_b + address, &v, 4); }
static void put16(uint32_t address, uint16_t v) { memcpy(memory_a + address, &v, 2); memcpy(memory_b + address, &v, 2); }
static void put8(uint32_t address, uint8_t v) { memory_a[address] = v; memory_b[address] = v; }
static void putf(uint32_t address, float v) { put32(address, fbits(v)); }
static float getf_a(uint32_t address) { float v; memcpy(&v, memory_a + address, 4); return v; }

static void print_cpu(const char *name, const EngineCPU *c) {
    printf("%s: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fl=%08x pc=%08x valid=%02x top=%u cw=%04x sw=%04x st0=%a\n",
           name, c->gpr[0], c->gpr[1], c->gpr[2], c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], c->gpr[7],
           c->flags, c->pc, c->fp_valid, c->fp_top, c->fp_control, c->fp_status, c->fp_reg[engine_fp_physical(c, 0)]);
}
/* Everything a caller can see. exact: also ST0, the condition bits and the
 * raw contents of empty x87 slots, which only the same instruction sequence
 * reproduces and nothing observes (FSAVE stores empty slots as zero). */
static int same_cpu(const EngineCPU *a, const EngineCPU *b, int exact) {
    uint32_t condition = exact ? 0u : 0x4500u;
    if (memcmp(a->gpr + 1, b->gpr + 1, sizeof a->gpr - 4) || ((a->gpr[0] ^ b->gpr[0]) & ~condition)) return 0;
    if (a->flags != b->flags || a->pc != b->pc || a->instruction_count != b->instruction_count) return 0;
    if (a->fp_valid != b->fp_valid || a->fp_top != b->fp_top || a->fp_control != b->fp_control) return 0;
    if ((a->fp_status ^ b->fp_status) & ~condition) return 0;
    for (unsigned i = exact ? 0 : 1; i < 8; i++)
        if ((exact || (a->fp_valid & (1u << i))) && dbits(a->fp_reg[engine_fp_physical(a, i)]) != dbits(b->fp_reg[engine_fp_physical(b, i)])) return 0;
    return 1;
}

static const uint32_t constant_address[] = { 0x00672ABCu, 0x00672B8Cu, 0x00672AC0u, 0x00672BACu, 0x00672BE0u };
static const float constant_retail[] = { 0.5f, 0.25f, 0.0f, 0.1f, FLT_MAX };

typedef struct { uint32_t object; int special; } Inputs;
static Inputs random_inputs(EngineCPU *cpu) {
    /* Globals, the object table and one object; sometimes non-retail constants. */
    Inputs in;
    put32(0x006F187Cu, GLOBALS);
    put8(GLOBALS + 9, (next32() % 3 == 0) ? (uint8_t)(1 + next32() % 255) : 0);
    put32(0x008603B0u, OBJECT_HEADER);
    put32(OBJECT_HEADER + 0x34, OBJECT_TABLE);
    unsigned index = next32() % 64, object = next32() % OBJECT_COUNT;
    in.object = object_at(object);
    put32(OBJECT_TABLE + index * 12 + 8, in.object);
    uint32_t flags = next32();
    flags = (next32() & 1) ? (flags | 0x00400000u) : (flags & ~0x00400000u);
    put32(in.object + 0x10, flags);
    in.special = memory_a[GLOBALS + 9] && (flags & 0x00400000u);
    for (unsigned k = 0; k < 3; k++) putf(in.object + 0xA0 + 4 * k, (next32() % 4) ? (float)((unit() - 0.5) * 400.0) : random_float());
    putf(in.object + 0xAC, (next32() % 4) ? (float)(unit() * 4.0) : random_float());
    static const uint16_t switches[] = { 2, 2, 2, 1, 0, 3, 0xFFFF };
    put16(0x00689450u, switches[next32() % (sizeof switches / sizeof *switches)]);
    for (unsigned i = 0; i < 5; i++) putf(constant_address[i], (next32() % 10 == 0) ? random_float() : constant_retail[i]);
    for (uint32_t address = 0x007C3184u; address <= 0x007C31A8u; address += 12) putf(address, random_float());
    putf(0x007C32F0u, (next32() % 8) ? (float)(100.0 + unit() * 1000.0) : random_float());
    /* Registers, flags, return address and an x87 stack with room for three. */
    memset(cpu, 0, sizeof *cpu);
    for (unsigned r = 0; r < 8; r++) cpu->gpr[r] = next32();
    cpu->gpr[0] = (next32() & 0xFFFF0000u) | index;
    cpu->gpr[4] = STACK + 0x100 + (next32() % 0x8000) * 4 + ((next32() % 16 == 0) ? 1 + next32() % 3 : 0);
    cpu->flags = (next32() & 0x00000ED5u) | 0x202u;
    cpu->pc = 0x0050F740u;
    put32(cpu->gpr[4], next32());
    unsigned depth = next32() % 6;
    for (unsigned i = 0; i < 8; i++) { double v; uint64_t b = next64(); memcpy(&v, &b, 8); cpu->fp_reg[engine_fp_physical(cpu, i)] = (next32() & 1) ? v : (double)random_float(); }
    cpu->fp_valid = (uint8_t)((1u << depth) - 1u);
    engine_fp_set_top(cpu, next32() & 7);
    static const uint16_t controls[] = { 0x027F, 0x027F, 0x027F, 0x037F, 0x067F, 0x0A7F, 0x0E7F, 0x007F };
    cpu->fp_control = controls[next32() % (sizeof controls / sizeof *controls)];
    cpu->fp_status = (uint16_t)next32();
    cpu->instruction_count = next64();
    return in;
}

/* 2 r S / max(|depth|, 0.1) with the original's operations, written out
 * independently of panorama_lod.h, for round-to-nearest. */
static double reference_pixels(const HaloPanoramaLod *lod, Inputs in, double scale) {
    if (in.special) return getf_a(0x00672BE0u);
    double radius = getf_a(in.object + 0xAC);
    uint16_t radius_switch; memcpy(&radius_switch, memory_a + 0x00689450u, 2);
    if (radius_switch == 1) radius *= getf_a(0x00672ABCu);
    else if (radius_switch == 0) radius *= getf_a(0x00672B8Cu);
    double v[3], depth;
    for (int k = 0; k < 3; k++) v[k] = (double)getf_a(in.object + 0xA0 + 4 * k) - (double)lod->head[k];
    if (lod->mode == HALO_PANORAMA_LOD_DISTANCE) {
        depth = sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]);
    } else {
        depth = -INFINITY;
        for (unsigned a = 0; a < lod->axes; a++) {
            double dot = ((double)lod->axis[a][0] * v[0] + (double)lod->axis[a][1] * v[1]) + (double)lod->axis[a][2] * v[2];
            if (isnan(dot)) { depth = dot; break; }
            if (dot > depth) depth = dot;
        }
    }
    double zero = getf_a(0x00672AC0u), clamp = getf_a(0x00672BACu);
    if (!(depth >= zero)) depth = -depth;          /* fcom: C0 is "less" or unordered */
    if (!(depth > clamp)) depth = clamp;           /* C0 or C3 */
    double quotient = scale / depth, product = radius * quotient;
    return product + product;
}

static unsigned part1(void) {
#if !HAVE_ORIGINAL
    puts("SKIP part 1: the generated engine tree is absent; the native original stands in below");
    return 0;
#else
    memory_a = calloc(MEMORY, 1); memory_b = calloc(MEMORY, 1); assert(memory_a && memory_b);
    uint8_t *before = malloc(MEMORY); assert(before);
    const unsigned iterations = 300000;
    unsigned full_checks = 0, special_paths = 0, clamps = 0, references = 0, differed = 0;
    for (unsigned n = 0; n < iterations; n++) {
        EngineCPU cpu; Inputs in = random_inputs(&cpu);
        int mode = n % 3;   /* BEARING, DISTANCE, NEAREST */
        HaloPanoramaLod lod; float pose[9]; random_pose(pose, (float)(unit() - 0.5));
        halo_panorama_lod_begin(&lod, mode, pose, passes, PASSES);
        if (next32() % 4 == 0) { lod.scale = (float)(200.0 + unit() * 800.0); lod.have_scale = 1; }
        double scale = lod.have_scale ? lod.scale : getf_a(0x007C32F0u);
        EngineCPU a = cpu, b = cpu;
        uint32_t esp = cpu.gpr[4];
        int whole = n < 96 || n % 4096 < 3;
        if (whole) memcpy(before, memory_a, MEMORY);
        engine_flat_base = memory_a; run_original(&a);
        engine_flat_base = memory_b; halo_panorama_lod_object_pixels(&b, mode == HALO_PANORAMA_LOD_BEARING ? NULL : &lod);
        if (!same_cpu(&a, &b, mode == HALO_PANORAMA_LOD_BEARING)) {
            printf("MISMATCH at iteration %u, mode %d\n", n, mode); print_cpu("original", &a); print_cpu("native  ", &b);
            return 1u << 31;
        }
        if (memcmp(memory_a + esp - 64, memory_b + esp - 64, 128)) { printf("STACK MISMATCH at iteration %u\n", n); return 1u << 31; }
        uint32_t return_address; memcpy(&return_address, memory_a + esp, 4);
        assert(a.pc == return_address && a.gpr[4] == esp + 4);
        if (whole) {
            /* Only the 12-byte position copy (and, on the special path, the
             * saved ESI) below the return address may change. */
            assert(!memcmp(memory_a, before, esp - 16) && !memcmp(memory_a + esp, before + esp, MEMORY - esp));
        }
        if (n % 256 == 0) { assert(!memcmp(memory_a, memory_b, MEMORY)); full_checks++; }
        if (mode != HALO_PANORAMA_LOD_BEARING && cpu.fp_control == 0x027F) {
            double expected = reference_pixels(&lod, in, scale);
            if (dbits(b.fp_reg[engine_fp_physical(&b, 0)]) != dbits(expected) && !(isnan(b.fp_reg[engine_fp_physical(&b, 0)]) && isnan(expected))) {
                printf("REFERENCE MISMATCH at iteration %u, mode %d: %a != %a\n", n, mode, b.fp_reg[engine_fp_physical(&b, 0)], expected);
                return 1u << 31;
            }
            references++;
            differed += dbits(a.fp_reg[engine_fp_physical(&a, 0)]) != dbits(b.fp_reg[engine_fp_physical(&b, 0)]);
        }
        special_paths += in.special;
        clamps += ((a.fp_status >> 8) & 0x41u) != 0;
    }
    assert(!memcmp(memory_a, memory_b, MEMORY));
    printf("part 1: over %u random calls against the translated 0050F740 (%u whole-memory comparisons, %u special-case, "
           "%u clamped): the original depth is bit-identical in every register, flag, x87 slot/tag/TOP/CW/SW, the return "
           "and memory; the frame's depth differs only in ST0 and C0/C2/C3, and ST0 matched an independent reference "
           "in %u of %u round-to-nearest calls (%u of them differing from the original)\n",
           iterations, full_checks, special_paths, clamps, references, references, differed);
    free(before); free(memory_a); free(memory_b);
    return iterations;
#endif
}

/* ---------------------------------------------------------------- part 2 */
/* What the engine derives from the pixel size: the model LOD and draw cutoff
 * (004D6FC0; five cutoffs per model, illustrative tables for a biped, a
 * larger character and a vehicle), the shadow and its fade from 30 to 45 px
 * and the shadow's silhouette LOD at 0.3 of the size (0050EBA0, 0050EE20),
 * and the lighting cadence at 400 and 100 px (0050F270). */
typedef struct { int lod, big_lod, big_drawn, vehicle_lod, shadow, silhouette, cadence; uint32_t fade; } Decisions;
static int lod_index(float pixels, const float cutoff[5]) {
    for (int i = 4; i >= 1; i--) if (!(pixels < cutoff[i])) return i;
    return 0;
}
static Decisions decide(float pixels) {
    static const float biped[5] = { 0, 50, 100, 150, 300 }, big[5] = { 5, 50, 120, 250, 400 }, vehicle[5] = { 0, 50, 80, 120, 250 };
    Decisions d; memset(&d, 0, sizeof d);
    d.lod = lod_index(pixels, biped);
    d.big_lod = lod_index(pixels, big); d.big_drawn = !(pixels < big[0]);
    d.vehicle_lod = lod_index(pixels, vehicle);
    d.shadow = pixels > 30.0f;
    double fade = ((double)pixels - 30.0) * (double)0.06666667f;
    fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
    d.fade = fbits((float)fade);
    d.silhouette = lod_index((float)((double)pixels * (double)0.3f), biped);
    d.cadence = pixels > 400.0f ? 1 : pixels > 100.0f ? 3 : 10;
    return d;
}
static int same_steps(Decisions a, Decisions b) {   /* all but the continuous fade */
    return a.lod == b.lod && a.big_lod == b.big_lod && a.big_drawn == b.big_drawn && a.vehicle_lod == b.vehicle_lod &&
           a.shadow == b.shadow && a.silhouette == b.silhouette && a.cadence == b.cadence;
}

typedef struct { float eye[3], forward[3], up[3], right[3]; float shift; } PassCamera;
/* A pass's camera, with the pass loop's operations (eye offset without roll). */
static PassCamera pass_camera(const float pose[9], int n, float separation) {
    PassCamera c; const float *f = pose + 3, *u = pose + 6;
    float right[3] = { f[1]*u[2]-f[2]*u[1], f[2]*u[0]-f[0]*u[2], f[0]*u[1]-f[1]*u[0] };
    float length = sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    int pass = passes[n].layer;
    c.shift = separation == 0.f ? 0.f : pass == HALO_PANORAMA_CENTRE_LEFT ? -separation
            : pass == HALO_PANORAMA_CENTRE_RIGHT ? separation : 0.f;
    for (int k = 0; k < 3; k++) c.eye[k] = pose[k] + right[k] / length * c.shift;
    halo_panorama_lod_bearing_axis(f, u, passes[n].yaw, passes[n].pitch, c.forward);
    float pitch = passes[n].pitch;
    if (pitch != 0.f) {
        float yawed[3]; halo_panorama_lod_bearing_axis(f, u, passes[n].yaw, 0.f, yawed);
        for (int k = 0; k < 3; k++) c.up[k] = u[k]*cosf(pitch) - yawed[k]*sinf(pitch);
    } else memcpy(c.up, u, sizeof c.up);
    float *fw = c.forward, *up = c.up;
    float rr[3] = { fw[1]*up[2]-fw[2]*up[1], fw[2]*up[0]-fw[0]*up[2], fw[0]*up[1]-fw[1]*up[0] };
    float rl = sqrtf(rr[0]*rr[0]+rr[1]*rr[1]+rr[2]*rr[2]);
    for (int k = 0; k < 3; k++) c.right[k] = rr[k] / rl;
    return c;
}
/* The view globals 0050CC40 and 0050BFB0 leave for 0050F740: the depth row of
 * the world-to-view matrix (the view's z axis is -forward), its offset, in
 * float as the engine stores them, and S. */
static void set_view(const float forward[3], const float eye[3], float scale) {
    const float *f = forward, *e = eye;
    setf(0x007C3184u, -f[0]); setf(0x007C3190u, -f[1]); setf(0x007C319Cu, -f[2]);
    setf(0x007C31A8u, f[0]*e[0] + f[1]*e[1] + f[2]*e[2]);
    setf(0x007C32F0u, scale);
}
/* Whether the pass's frustum (DENSE, as on the headset: 32 degrees either
 * side for the ring, 48 for the caps, 52.5 up and down) reaches the sphere. */
static int pass_draws(const PassCamera *c, int n, const float p[3], float radius) {
    float v[3] = { p[0]-c->eye[0], p[1]-c->eye[1], p[2]-c->eye[2] };
    float z = v[0]*c->forward[0]+v[1]*c->forward[1]+v[2]*c->forward[2];
    float x = v[0]*c->right[0]+v[1]*c->right[1]+v[2]*c->right[2];
    float y = v[0]*c->up[0]+v[1]*c->up[1]+v[2]*c->up[2];
    int cap = passes[n].layer == HALO_PANORAMA_UP || passes[n].layer == HALO_PANORAMA_DOWN;
    float h = (cap ? 48.f : 32.f) * (float)M_PI / 180.f, vv = 52.5f * (float)M_PI / 180.f;
    return z > -radius && fabsf(x) <= z * tanf(h) + radius / cosf(h) && fabsf(y) <= z * tanf(vv) + radius / cosf(vv);
}

static void setup_object_table(void) {
    S32(0x006F187Cu, GLOBALS); S8(GLOBALS + 9, 0);
    S32(0x008603B0u, OBJECT_HEADER); S32(OBJECT_HEADER + 0x34, OBJECT_TABLE);
    for (unsigned i = 0; i < OBJECT_COUNT; i++) { S32(OBJECT_TABLE + i * 12 + 8, object_at(i)); S32(object_at(i) + 0x10, 0); }
    S16(0x00689450u, 2);
    for (unsigned i = 0; i < 5; i++) setf(constant_address[i], constant_retail[i]);
}
static float call_pixels(unsigned object, HaloPanoramaLod *lod, int original) {
    EngineCPU cpu; memset(&cpu, 0, sizeof cpu);
    cpu.gpr[4] = STACK + 0x8000; cpu.gpr[0] = 0xE1230000u | object; cpu.fp_control = 0x027F; cpu.flags = 0x202;
    S32(cpu.gpr[4], 0x0050EEE3u);
    if (original) run_original(&cpu); else halo_panorama_lod_object_pixels(&cpu, lod);
    assert(cpu.pc == 0x0050EEE3u && cpu.gpr[4] == STACK + 0x8004 && cpu.fp_valid == 1);
    return (float)cpu.fp_reg[engine_fp_physical(&cpu, 0)];   /* the callers' fstp dword */
}
static double cos_between(const double a[3], const float b[3]) {
    double d = a[0]*b[0]+a[1]*b[1]+a[2]*b[2], la = sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]), lb = sqrt((double)b[0]*b[0]+(double)b[1]*b[1]+(double)b[2]*b[2]);
    return d / (la * lb);
}
/* A bound on the original's own rounding, relative: its depth comes from
 * float view globals, whose offset f.e carries up to |e| * 2^-24, and its
 * result is stored as a float. Four times that, to be safe. */
static double original_rounding(const float eye[3], double length, double planar) {
    return ((fabs((double)eye[0]) + fabs((double)eye[1]) + fabs((double)eye[2]) + length) / planar + 1.0) * 0x1.0p-22;
}

enum { FRAMES = 60, OBJECTS_PER_FRAME = 1000 };
static int part2(void) {
    engine_flat_base = calloc(MEMORY, 1); assert(engine_flat_base);
    setup_object_table();
    const float separation = 0.0103346457f;                    /* 63 mm IPD in world units */
    const float scale = (float)(0.5 * 1536.0 / tan(0.5 * 105.0 * M_PI / 180.0));
    unsigned multi = 0, split_step = 0, split_lod = 0, split_shadow = 0, split_cadence = 0, split_fade = 0;
    unsigned belt = 0, belt_split = 0, eye_pairs = 0, eye_split = 0;
    unsigned home = 0, home_steps_distance = 0, home_lod_distance = 0, home_shadow_distance = 0, home_cadence_distance = 0;
    unsigned home_steps_nearest = 0, axis_objects = 0, level = 0, level_steps = 0;
    double level_ratio_sum = 0;
    double home_ratio_min = 1, home_ratio_sum = 0, cos_error = 0, axis_error = 0, stereo_cos_error = 0;
    double nearest_home_error = 0, nearest_over = 0, worst_rounding = 0;
    static float positions[OBJECTS_PER_FRAME][3], radii[OBJECTS_PER_FRAME];
    static float original[PASSES][OBJECTS_PER_FRAME], distance[PASSES][OBJECTS_PER_FRAME], nearest[PASSES][OBJECTS_PER_FRAME];
    static int draws[PASSES][OBJECTS_PER_FRAME];
    for (unsigned frame = 0; frame < FRAMES; frame++) {
        float pose[9]; random_pose(pose, frame % 4 == 3 ? 0.3f : 0.f);
        HaloPanoramaLod frame_distance, frame_nearest;
        halo_panorama_lod_begin(&frame_distance, HALO_PANORAMA_LOD_DISTANCE, pose, passes, PASSES);
        halo_panorama_lod_begin(&frame_nearest, HALO_PANORAMA_LOD_NEAREST, pose, passes, PASSES);
        const float *f = pose + 3, *u = pose + 6;
        float r[3] = { f[1]*u[2]-f[2]*u[1], f[2]*u[0]-f[0]*u[2], f[0]*u[1]-f[1]*u[0] };
        float rl = sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); for (int k = 0; k < 3; k++) r[k] /= rl;
        for (unsigned i = 0; i < OBJECTS_PER_FRAME; i++) {
            /* Uniform directions 0.05 to 200 units away; a fifth on a ring
             * seam or in the band/cap belt, and a few on the centre axis. */
            double z = unit() * 2 - 1, a = unit() * 2 * M_PI, s = sqrt(1 - z * z);
            double dir[3] = { s * cos(a), s * sin(a), z }, d = 0.05 * pow(4000.0, unit());
            if (i % 5 == 0 || i % 50 == 1) {
                double yaw = (i % 10 == 0) ? (M_PI / 6) * (2 * (next32() % 6) + 1) + (unit() - 0.5) * 0.07 : unit() * 2 * M_PI;
                double el = (i % 10 == 0) ? (unit() - 0.5) * 1.4 : (0.65 + unit() * 0.27) * ((next32() & 1) ? 1 : -1);
                if (i % 50 == 1) { yaw = (unit() - 0.5) * 1e-5; el = (unit() - 0.5) * 1e-5; }
                for (int k = 0; k < 3; k++) dir[k] = cos(el) * (cos(yaw) * f[k] + sin(yaw) * r[k]) + sin(el) * u[k];
            }
            for (int k = 0; k < 3; k++) positions[i][k] = (float)(pose[k] + dir[k] * d);
            radii[i] = (float)(0.05 + unit() * 2.0);
            memcpy(GPTR(object_at(i) + 0xA0), positions[i], 12); setf(object_at(i) + 0xAC, radii[i]);
        }
        PassCamera cameras[PASSES];
        for (int n = 0; n < PASSES; n++) {
            cameras[n] = pass_camera(pose, n, separation);
            set_view(cameras[n].forward, cameras[n].eye, scale);
            for (unsigned i = 0; i < OBJECTS_PER_FRAME; i++) {
                original[n][i] = call_pixels(i, NULL, 1);
                distance[n][i] = call_pixels(i, &frame_distance, 0);
                nearest[n][i] = call_pixels(i, &frame_nearest, 0);
                draws[n][i] = pass_draws(&cameras[n], n, positions[i], radii[i]);
            }
        }
        for (unsigned i = 0; i < OBJECTS_PER_FRAME; i++) {
            /* One number in every pass and both eyes. */
            for (int n = 1; n < PASSES; n++) {
                assert(fbits(distance[n][i]) == fbits(distance[0][i]));
                assert(fbits(nearest[n][i]) == fbits(nearest[0][i]));
            }
            double v[3] = { (double)positions[i][0]-pose[0], (double)positions[i][1]-pose[1], (double)positions[i][2]-pose[2] };
            double length = sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
            /* The original's disagreements between the passes that draw it. */
            int drawn = 0, have = 0, step = 0, lod = 0, shadow = 0, cadence = 0, fade = 0, ring = 0, cap = 0;
            Decisions first; memset(&first, 0, sizeof first);
            for (int n = 0; n < PASSES; n++) {
                if (!draws[n][i]) continue;
                drawn++;
                if (passes[n].layer == HALO_PANORAMA_UP || passes[n].layer == HALO_PANORAMA_DOWN) cap = 1; else ring = 1;
                Decisions o = decide(original[n][i]);
                if (!have) { first = o; have = 1; continue; }
                step |= !same_steps(o, first);
                lod |= o.lod != first.lod || o.big_lod != first.big_lod || o.vehicle_lod != first.vehicle_lod || o.big_drawn != first.big_drawn;
                shadow |= o.shadow != first.shadow || o.silhouette != first.silhouette;
                cadence |= o.cadence != first.cadence;
                fade |= o.fade != first.fade;
            }
            for (int n = 0; n < PASSES; n++) {
                if (!draws[n][i] || !(original[n][i] > 0)) continue;
                double w[3] = { positions[i][0]-(double)cameras[n].eye[0], positions[i][1]-(double)cameras[n].eye[1], positions[i][2]-(double)cameras[n].eye[2] };
                double planar = fabs(w[0]*cameras[n].forward[0]+w[1]*cameras[n].forward[1]+w[2]*cameras[n].forward[2]);
                if (planar > 0.1) nearest_over = fmax(nearest_over, ((double)nearest[n][i] / original[n][i] - 1.0) /
                                                                    (original_rounding(cameras[n].eye, length, planar) + fabs(cameras[n].shift) / planar));
            }
            if (drawn >= 2) { multi++; split_step += step; split_lod += lod; split_shadow += shadow; split_cadence += cadence; split_fade += fade; }
            if (ring && cap) { belt++; belt_split += step; }
            if (draws[CENTRE_LEFT_PASS][i] && draws[CENTRE_RIGHT_PASS][i]) {
                eye_pairs++;
                eye_split += !same_steps(decide(original[CENTRE_LEFT_PASS][i]), decide(original[CENTRE_RIGHT_PASS][i]));
            }
            /* The centre bearing's own sector (|yaw| <= 30, below the belt),
             * against a mono centre pass from the head and both eyes. */
            double forward_part = v[0]*f[0]+v[1]*f[1]+v[2]*f[2], right_part = v[0]*r[0]+v[1]*r[1]+v[2]*r[2];
            double up_part = v[0]*u[0]+v[1]*u[1]+v[2]*u[2];
            double yaw = atan2(right_part, forward_part), elevation = asin(fmax(-1, fmin(1, up_part / length)));
            if (fabs(yaw) > M_PI / 6 || fabs(elevation) > 37.5 * M_PI / 180 || forward_part <= 0.1) continue;
            home++;
            PassCamera mono = pass_camera(pose, CENTRE_LEFT_PASS, 0.f);
            set_view(mono.forward, mono.eye, scale);
            float mono_original = call_pixels(i, NULL, 1);
            double planar = fabs(v[0]*mono.forward[0]+v[1]*mono.forward[1]+v[2]*mono.forward[2]);
            double rounding = original_rounding(mono.eye, length, planar);
            if (planar > 0.1) worst_rounding = fmax(worst_rounding, fabs(2.0*radii[i]*scale/planar / mono_original - 1.0) / rounding);
            double ratio = (double)distance[0][i] / mono_original, c = cos_between(v, mono.forward);
            home_ratio_min = fmin(home_ratio_min, ratio); home_ratio_sum += ratio;
            assert(ratio <= 1.0 + 2.0 * rounding);   /* never more detail than the original */
            if (planar > 0.1) cos_error = fmax(cos_error, fabs(ratio - c) / rounding);
            if (c > cos(1e-4) && planar > 0.1) { axis_objects++; axis_error = fmax(axis_error, fabs(ratio - 1) / rounding); }
            for (int n = CENTRE_RIGHT_PASS; n <= CENTRE_LEFT_PASS; n++) if (planar > 0.1)
                stereo_cos_error = fmax(stereo_cos_error, fabs((double)distance[n][i] / original[n][i] - c) / (rounding + separation / planar));
            Decisions o = decide(mono_original), dd = decide(distance[0][i]);
            home_steps_distance += !same_steps(o, dd);
            if (fabs(elevation) <= 15.0 * M_PI / 180) { level++; level_steps += !same_steps(o, dd); level_ratio_sum += ratio; }
            home_lod_distance += o.lod != dd.lod || o.big_lod != dd.big_lod || o.vehicle_lod != dd.vehicle_lod || o.big_drawn != dd.big_drawn;
            home_shadow_distance += o.shadow != dd.shadow || o.silhouette != dd.silhouette;
            home_cadence_distance += o.cadence != dd.cadence;
            if (planar > 0.1) nearest_home_error = fmax(nearest_home_error, fabs((double)nearest[0][i] / mono_original - 1.0) / rounding);
            home_steps_nearest += !same_steps(o, decide(nearest[0][i])) && planar > 1.0;
        }
    }
    /* The original's float view globals are the only error source. */
    assert(worst_rounding <= 1.0);
    assert(cos_error <= 2.0 && axis_error <= 2.0 && stereo_cos_error <= 2.0);
    assert(nearest_home_error <= 2.0 && nearest_over <= 1.0);
    printf("part 2: %d frames x %d objects x %d passes (stereo, 63 mm, DENSE frusta): DISTANCE and NEAREST give each object one "
           "pixel size in every pass and both eyes, so its LOD, cutoff, shadow, fade, silhouette LOD and lighting cadence agree "
           "wherever it is drawn\n", FRAMES, OBJECTS_PER_FRAME, PASSES);
    printf("  original: %u of %u objects drawn by two or more passes (%.1f%%) got a different LOD/cutoff/shadow/cadence step "
           "(LOD %u, shadow or silhouette %u, lighting cadence %u) and %u a different shadow fade; band/cap belt %u of %u "
           "(%.1f%%); centre eyes %u of %u\n",
           split_step, multi, 100.0 * split_step / multi, split_lod, split_shadow, split_cadence, split_fade,
           belt_split, belt, 100.0 * belt_split / (belt ? belt : 1), eye_split, eye_pairs);
    printf("  centre bearing's own sector (%u objects), DISTANCE against the original: ratio = cos(angle off the axis) to "
           "within %.2f of the original's own float rounding (stereo eyes %.2f), exact on the axis (%u objects, %.2f); "
           "ratio %.3f..1, mean %.3f; a step changed for %.1f%% (LOD %.1f%%, shadow/silhouette %.1f%%, cadence %.1f%%), "
           "always to the lighter side; within 15 degrees of level (%u objects) mean %.3f, %.1f%%\n",
           home, cos_error, stereo_cos_error, axis_objects, axis_error, home_ratio_min, home_ratio_sum / home,
           100.0 * home_steps_distance / home, 100.0 * home_lod_distance / home, 100.0 * home_shadow_distance / home,
           100.0 * home_cadence_distance / home, level, level_ratio_sum / level, 100.0 * level_steps / level);
    printf("  NEAREST: the original over the centre sector to within %.2f of its rounding (%u steps changed beyond 1 unit), "
           "never above the original in any pass that draws the object beyond its rounding (worst %.2f of it)\n",
           nearest_home_error, home_steps_nearest, nearest_over);
    free(engine_flat_base);
    return 0;
}

/* ---------------------------------------------------------------- part 3 */
static const uint32_t stack_address = 0x10000, renderer_address = 0x20000, caller_address = 0x00401234u;
static float frame_pixels[HALO_PANORAMA_LAYERS][64];
static unsigned frame_hooked, frame_passes, frame_objects;
static int frame_expect_hook;
static float frame_pose[9];

/* One bearing pass as the engine would run it: 0050BFB0's view globals from
 * the pass's frustum and S from its rectangle and field, as 0050CC40 derives
 * it, then 0050F740 for each object through the dispatch hook, then the
 * mirror pass, a foreign camera and a full x87 stack. Each call is also run
 * through the original on a copy of the guest state. */
static void fake_pass(EngineCPU *cpu) {
    uint32_t frustum = renderer_address + 4;
    memcpy(GPTR(0x007C3114u), GPTR(frustum), 0x54);
    S16(0x007C3108u, G16(renderer_address));
    float position[3], forward[3], fov; memcpy(position, GPTR(frustum), 12); memcpy(forward, GPTR(frustum + 0x0C), 12);
    memcpy(&fov, GPTR(frustum + 0x28), 4);
    int16_t rect[4]; memcpy(rect, GPTR(frustum + 0x2C), sizeof rect);
    set_view(forward, position, (float)(0.5 * (rect[2] - rect[0]) / tan(0.5 * fov)));
    for (unsigned i = 0; i < frame_objects; i++) {
        EngineCPU call; memset(&call, 0, sizeof call);
        call.gpr[0] = 0xBEEF0000u | i; call.gpr[1] = 0x11111111u; call.gpr[3] = 0x33333333u; call.gpr[5] = 0x55555555u;
        call.gpr[6] = 0x66666666u; call.gpr[7] = 0x77777777u; call.gpr[4] = stack_address + 0x400; call.flags = 0x246;
        call.fp_control = 0x027F; call.fp_status = 0x0100; call.fp_reg[engine_fp_physical(&call, 0)] = 5.0; call.fp_reg[engine_fp_physical(&call, 1)] = -7.0; call.fp_valid = 3;
        S32(call.gpr[4], 0x0050EC17u);
        EngineCPU reference = call;
        uint32_t entry = call.gpr[4];
        memset(GPTR(entry - 32), 0xA5, 32);
        unsigned char window[64]; memcpy(window, GPTR(entry - 32), sizeof window);
        int hooked = host_panorama_dispatch(&call, 0x0050F740u);
        if (!hooked) run_original(&call);
        unsigned char hooked_window[64]; memcpy(hooked_window, GPTR(entry - 32), sizeof hooked_window);
        memcpy(GPTR(entry - 32), window, sizeof window);
        run_original(&reference);
        /* The same guest state apart from ST0 and the condition bits (and
         * AX, which holds the status word) that compared it. */
        assert(!memcmp(hooked_window, GPTR(entry - 32), sizeof hooked_window));
        assert(memcmp(hooked_window, window, sizeof window));   /* it did write below the return address */
        assert(call.pc == 0x0050EC17u && same_cpu(&call, &reference, !hooked));
        frame_hooked += hooked;
        assert(hooked == frame_expect_hook);
        frame_pixels[panorama_view_pass][i] = (float)call.fp_reg[engine_fp_physical(&call, 0)];
    }
    /* The mirror pass (view -1, a reflected camera), a camera that is not this
     * frame's head or an eye beside it, and an x87 stack with no room for the
     * original's temporaries keep the original. */
    EngineCPU call; memset(&call, 0, sizeof call);
    call.gpr[4] = stack_address + 0x400; call.fp_control = 0x027F; S32(call.gpr[4], 0x0050EC17u);
    S16(0x007C3108u, 0xFFFF);
    assert(host_panorama_dispatch(&call, 0x0050F740u) == 0);
    S16(0x007C3108u, 0);
    float away[3] = { frame_pose[0] + 0.5f, frame_pose[1], frame_pose[2] };
    memcpy(GPTR(0x007C3114u), away, 12);
    assert(host_panorama_dispatch(&call, 0x0050F740u) == 0);
    memcpy(GPTR(0x007C3114u), GPTR(frustum), 12);
    call.fp_valid = 0x3F;
    assert(host_panorama_dispatch(&call, 0x0050F740u) == 0);
    frame_passes++;
    cpu->gpr[4] += 4; cpu->pc = G32(stack_address);
}
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    assert(address == 0x0050BEA0u);
    assert(host_panorama_dispatch(cpu, address) == 0);
    fake_pass(cpu);
}

static void run_frame(const char *mode, int expect_hook, unsigned expect_passes) {
    if (mode) setenv("HALO_PANORAMA_LOD", mode, 1); else unsetenv("HALO_PANORAMA_LOD");
    frame_expect_hook = expect_hook; frame_hooked = frame_passes = 0;
    memset(frame_pixels, 0, sizeof frame_pixels);
    S32(stack_address, caller_address); S32(stack_address + 4, renderer_address); S32(stack_address + 8, 1);
    S32(stack_address + 12, 0x123450); S32(stack_address + 16, 0x3F800000); S32(stack_address + 20, 0x3D088889);
    for (unsigned frustum = 0; frustum < 2; frustum++) {
        uint32_t at = renderer_address + (frustum ? 0x58 : 4);
        memcpy(GPTR(at), frame_pose, 36);
        float fov = 1.2f; memcpy(GPTR(at + 0x28), &fov, 4);
        int16_t rect[4] = { 0, 0, 1536, 1536 }; memcpy(GPTR(at + 0x2C), rect, sizeof rect);
    }
    S16(renderer_address, 0);
    EngineCPU cpu; memset(&cpu, 0, sizeof cpu);
    cpu.gpr[4] = stack_address; cpu.pc = 0x0050BEA0u; cpu.fp_control = 0x027F; cpu.flags = 0x202;
    assert(host_panorama_dispatch(&cpu, 0x0050BEA0u) == 1);
    assert(cpu.pc == caller_address && frame_passes == expect_passes);
    /* Outside a panorama frame the original always runs. */
    EngineCPU call; memset(&call, 0, sizeof call); call.gpr[4] = stack_address + 0x400; S32(call.gpr[4], 0x0050EC17u);
    assert(host_panorama_dispatch(&call, 0x0050F740u) == 0);
}

static const int layers[PASSES] = { 0, 2, HALO_PANORAMA_UP, HALO_PANORAMA_DOWN, HALO_PANORAMA_REAR_RIGHT,
                                    HALO_PANORAMA_REAR, HALO_PANORAMA_REAR_LEFT, HALO_PANORAMA_CENTRE_RIGHT, HALO_PANORAMA_CENTRE_LEFT };
static int part3(void) {
    engine_flat_base = calloc(MEMORY, 1); assert(engine_flat_base);
    setup_object_table();
    setenv("HALO_PANORAMA", "1", 1);
    setenv("HALO_PANORAMA_TIGHT_BUDGET", "0", 1);   /* stereo at every tier */
    unsetenv("HALO_PANORAMA_VIEWS");
    unsetenv("HALO_STEREO");
    assert(halo_settings_stereo_separation() > 0.f);
    random_pose(frame_pose, 0.f);
    frame_objects = 64;
    for (unsigned i = 0; i < frame_objects; i++) {
        float p[3]; for (int k = 0; k < 3; k++) p[k] = frame_pose[k] + (float)((unit() - 0.5) * 40.0);
        memcpy(GPTR(object_at(i) + 0xA0), p, 12); setf(object_at(i) + 0xAC, (float)(0.1 + unit()));
    }
    memcpy(GPTR(object_at(5) + 0xA0), frame_pose, 12);            /* one at the head: the 0.1 clamp */
    {   /* One in the band/cap belt, drawn by the centre and the sky: straight
         * ahead, 42 degrees up, 12 units away, a small biped's radius. */
        const float *f = frame_pose + 3, *u = frame_pose + 6; float p[3];
        for (int k = 0; k < 3; k++) p[k] = frame_pose[k] + 12.f * (cosf(0.73303829f) * f[k] + sinf(0.73303829f) * u[k]);
        memcpy(GPTR(object_at(3) + 0xA0), p, 12); setf(object_at(3) + 0xAC, 0.212f);
    }
    S32(object_at(7) + 0x10, 0x00400000u); S8(GLOBALS + 9, 1);   /* one on the FLT_MAX path */
    const float expected_scale = (float)(0.5 * 1536.0 / tan(0.5 * halo_settings_panorama_vfov()));
    const char *modes[] = { NULL, "nearest" };
    for (int stereo = 1; stereo >= 0; stereo--) {
        HaloSettings settings; halo_settings_get(&settings);
        settings.stereo_separation = stereo ? 0.0103346457f : 0.f; halo_settings_set(&settings);
        for (int m = 0; m < 2; m++) {
            run_frame(modes[m], 1, stereo ? PASSES : PASSES - 1);
            assert(frame_hooked == frame_passes * frame_objects);
            for (unsigned i = 0; i < frame_objects; i++)
                for (int n = 1; n < PASSES; n++) {
                    if (!stereo && layers[n] == HALO_PANORAMA_CENTRE_RIGHT) continue;
                    assert(fbits(frame_pixels[layers[n]][i]) == fbits(frame_pixels[layers[0]][i]));
                }
            /* The frame's state: the head before the eye offset, and the
             * centre view's S (every pass's, 0.5 h / tan(vfov / 2)). */
            HaloPanoramaLod expected;
            halo_panorama_lod_begin(&expected, m ? HALO_PANORAMA_LOD_NEAREST : HALO_PANORAMA_LOD_DISTANCE, frame_pose, passes, PASSES);
            assert(panorama_lod[0].mode == expected.mode && !memcmp(panorama_lod[0].head, frame_pose, 12));
            assert(panorama_lod[0].axes == expected.axes && panorama_lod[0].axes == (m ? (unsigned)PASSES : 0u));
            assert(!memcmp(panorama_lod[0].axis, expected.axis, expected.axes * sizeof *expected.axis));
            assert(panorama_lod[0].have_scale && panorama_lod[0].scale == expected_scale);
            assert(frame_pixels[layers[0]][7] == FLT_MAX);
            if (!m) {
                /* Straight-line distance from the head times the centre S. */
                for (unsigned i = 0; i < frame_objects; i++) {
                    if (i == 7) continue;
                    float p[3]; memcpy(p, GPTR(object_at(i) + 0xA0), 12);
                    double v[3] = { (double)p[0]-frame_pose[0], (double)p[1]-frame_pose[1], (double)p[2]-frame_pose[2] };
                    double d = fmax(sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]), (double)0.1f), radius; float rf;
                    memcpy(&rf, GPTR(object_at(i) + 0xAC), 4); radius = rf;
                    assert(fabs(frame_pixels[layers[0]][i] / (2.0 * radius * expected_scale / d) - 1.0) < 1e-6);
                }
            }
        }
    }
    HaloSettings settings; halo_settings_get(&settings); settings.stereo_separation = 0.0103346457f; halo_settings_set(&settings);
    run_frame("bearing", 0, PASSES);
    assert(frame_hooked == 0);
    float left = frame_pixels[HALO_PANORAMA_CENTRE_LEFT][3], right = frame_pixels[HALO_PANORAMA_CENTRE_RIGHT][3];
    float sky = frame_pixels[HALO_PANORAMA_UP][3];
    run_frame("nearest", 1, PASSES);
    float owner = frame_pixels[HALO_PANORAMA_UP][3];
    run_frame(NULL, 1, PASSES);
    float invariant = frame_pixels[HALO_PANORAMA_UP][3];
    printf("part 3: in the pass loop every pass and both eyes got the same value for all %u objects, stereo and mono, "
           "DISTANCE and NEAREST, with the centre view's S (%.2f); each call matched the original's guest state apart from ST0 "
           "and C0/C2/C3; the mirror pass, a foreign camera, a full x87 stack and HALO_PANORAMA_LOD=bearing kept the original. "
           "An object 42 degrees up, 12 units ahead: originally %.2f px in both centre eyes (no shadow) and %.2f px in the sky "
           "pass (shadow); now %.2f px in every pass (NEAREST %.2f)\n",
           frame_objects, expected_scale, left, sky, invariant, owner);
    assert(fabsf(left / right - 1.f) < 1e-5f && left < 30.f && sky > 30.f);
    assert(fabsf(invariant * 12.f / (2.f * 0.212f * expected_scale) - 1.f) < 1e-5f);
    assert(fabsf(owner / left - 1.f) < 1e-5f);   /* the band owns 42 degrees: cos 42 > sin 42 */
    free(engine_flat_base);
    return 0;
}

/* ---------------------------------------------------------------- part 4 */
static void part4(void) {
    engine_flat_base = calloc(MEMORY, 1); assert(engine_flat_base);
    setup_object_table();
    float pose[9]; random_pose(pose, 0.f);
    for (unsigned i = 0; i < 256; i++) {
        float p[3]; for (int k = 0; k < 3; k++) p[k] = pose[k] + (float)((unit() - 0.5) * 60.0);
        memcpy(GPTR(object_at(i) + 0xA0), p, 12); setf(object_at(i) + 0xAC, (float)(0.1 + unit()));
    }
    PassCamera c = pass_camera(pose, 0, 0.f); set_view(c.forward, c.eye, 589.f);
    memcpy(GPTR(0x007C3114u), pose, 12); S16(0x007C3108u, 0);
    HaloPanoramaLod lod_distance, lod_nearest;
    halo_panorama_lod_begin(&lod_distance, HALO_PANORAMA_LOD_DISTANCE, pose, passes, PASSES);
    halo_panorama_lod_begin(&lod_nearest, HALO_PANORAMA_LOD_NEAREST, pose, passes, PASSES);
    /* The hook's own state for version 4: inside a panorama frame, mono. */
    panorama_nested = 1; panorama_renderer_count = 1; panorama_lod[0] = lod_distance; panorama_lod_reach = 0.f;
    static const char *names[] = { "translated 0050F740", "native, original depth", "native, DISTANCE",
                                   "native, NEAREST", "DISTANCE through host_panorama_dispatch" };
    const unsigned calls = 4000000;
    double seconds[5] = { 0 }; volatile double sink = 0;
    for (int version = 0; version < 5; version++) {
        if (version == 0 && !HAVE_ORIGINAL) continue;
        EngineCPU cpu; memset(&cpu, 0, sizeof cpu); cpu.fp_control = 0x027F;
        S32(STACK + 0x8000, 0x0050EEE3u);
        double start = now();
        for (unsigned n = 0; n < calls; n++) {
            cpu.gpr[4] = STACK + 0x8000; cpu.gpr[0] = n & 255u;
            if (version == 0) run_original(&cpu);
            else if (version == 4) { if (!host_panorama_dispatch(&cpu, 0x0050F740u)) abort(); }
            else halo_panorama_lod_object_pixels(&cpu, version == 1 ? NULL : version == 2 ? &lod_distance : &lod_nearest);
            sink += cpu.fp_reg[engine_fp_physical(&cpu, 0)]; engine_fp_pop(&cpu);
        }
        seconds[version] = now() - start;
    }
    (void)sink;
    panorama_nested = 0; panorama_renderer_count = 0;
    printf("part 4 (ns per call, %u calls each):", calls);
    for (int version = 0; version < 5; version++) {
        if (version == 0 && !HAVE_ORIGINAL) { printf(" %s absent;", names[0]); continue; }
        printf(" %s %.1f%s", names[version], seconds[version] * 1e9 / calls, version < 4 ? ";" : "\n");
    }
    free(engine_flat_base);
}

int main(void) {
    unsigned proven = part1();
    if (proven & (1u << 31)) return 1;
    if (part2()) return 1;
    if (part3()) return 1;
    part4();
    printf("PASS: one object pixel size per panorama frame%s\n",
           HAVE_ORIGINAL ? ", native original bit-identical to the translated 0050F740" : " (translated comparison skipped)");
    return 0;
}
