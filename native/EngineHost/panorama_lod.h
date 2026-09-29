#ifndef HALO_PANORAMA_LOD_H
#define HALO_PANORAMA_LOD_H
#include "engine_cpu.h"
#include "engine_flags.h"
#include "panorama_budget.h"
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* One level of detail per object per frame, the same in every bearing.
 *
 * 0050F740 is the pixel size Halo gives an object: 2 * r * S / depth, with r
 * the object's bounding radius (obj+0xAC, halved or quartered by the LOD
 * switch at 0x689450), S the view's pixels per unit at unit depth (0x7C32F0)
 * and depth the object's distance along this view's forward axis, at least
 * 0.1. That one number picks the model LOD and the cutoff below which a model
 * is not drawn (004D6FC0), admits an object to the shadow pass above 30 px and
 * fades its shadow in up to 45 px (0050EBA0), sets the shadow's silhouette LOD
 * at 0.3 of it, and picks the object's lighting record and how often it is
 * refreshed (0050EA00, 0050F150, 0050F270). Its only callers are 0050EBA0
 * (0050EBFA, 0050EC12, 0050ED82) and 0050EE20 (0050EEDE); each stores ST0 as a
 * float straight away and reads nothing else it leaves.
 *
 * The panorama draws each bearing with its own forward axis, so the same
 * object gets a different depth, and so a different pixel size, in every
 * bearing and in each eye. Where two bearings overlap (the band and cap belt
 * between 37.5 and 52.5 degrees of elevation, the ring's guard columns and
 * cross-fades) the object is drawn with two LODs, a shadow in one and none in
 * the other, and lighting refreshed on two cadences: a pop at the seam.
 *
 * Here every world pass of a frame measures an object from the head centre,
 * the game camera before the eye offset, so the value does not depend on the
 * bearing or the eye that asks:
 *
 *  - DISTANCE, the default: the straight-line distance, so the pixel size is
 *    the object's true angular size times the centre view's S. On the centre
 *    view's axis that is the original number. Off the axis the original
 *    divided by the planar depth, which is shorter by cos(angle), so the
 *    object now gets cos(angle) of its old pixel size in the bearing that
 *    shows it: 0.87 at a band's side edge, 0.79 at 37.5 degrees up, and the
 *    same number in every other bearing that draws it. It does not change as
 *    the view turns, only as the object or the player moves.
 *  - NEAREST (HALO_PANORAMA_LOD=nearest): the depth along the axis of the
 *    bearing nearest the object, the largest of its depths along the eight
 *    bearing axes. That is the original number in the bearing whose axis is
 *    nearest the object and no more than the original in any other bearing
 *    that draws it, so each bearing keeps PC LOD over its own sector; like
 *    the original, it changes as the view turns.
 *  - BEARING (HALO_PANORAMA_LOD=bearing): the original, for comparison.
 *
 * S is the first world pass's 0x7C32F0 of the frame. Every bearing has the
 * same one: 0050CC40 stores S = h / (2 tan(vfov / 2)) at record +0x188 from
 * the raster height and the vertical field, which the pass loop gives every
 * bearing alike (DENSE narrows only the projection's width), and 0050BFB0
 * copies the record to 0x7C3168. So it is the centre view's S; holding it for
 * the frame only makes that true by construction.
 *
 * Everything else is the original's, in its order: the 0x400000 special case
 * and its FLT_MAX, the radius switch, the 12-byte copy of the position on the
 * stack, the sign test and 0.1 clamp on the depth (a NaN takes the clamp, as
 * it did), the same x87 operations and stack depth, EAX/ECX/EDX, EFLAGS and
 * the status word. With the original depth (BEARING, or lod NULL) the routine
 * is bit-identical to the translated 0050F740 (tests/test_panorama_lod.c);
 * with the frame's depth only ST0 and the condition bits that compared it
 * (C0/C2/C3, and AX, which holds the status word) can differ. */
enum { HALO_PANORAMA_LOD_BEARING = 0, HALO_PANORAMA_LOD_DISTANCE = 1, HALO_PANORAMA_LOD_NEAREST = 2 };
enum { HALO_PANORAMA_LOD_AXES = 12 };

typedef struct {
    int mode;
    float head[3];
    float axis[HALO_PANORAMA_LOD_AXES][3];
    unsigned axes;
    float scale;
    int have_scale;
} HaloPanoramaLod;

/* The forward axis the pass loop gives a bearing, with the same float
 * operations: yaw about the head's up through its right vector, then the
 * whole basis pitched for the caps. */
static inline void halo_panorama_lod_bearing_axis(const float forward[3], const float up[3],
                                                  float angle, float pitch, float out[3]) {
    float f[3] = { forward[0], forward[1], forward[2] }, u[3] = { up[0], up[1], up[2] }, right[3];
    right[0] = f[1]*u[2]-f[2]*u[1];
    right[1] = f[2]*u[0]-f[0]*u[2];
    right[2] = f[0]*u[1]-f[1]*u[0];
    float length = sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (length > 0.0001f) for (int k=0;k<3;k++) f[k] = f[k]*cosf(angle)+right[k]/length*sinf(angle);
    if (pitch != 0.f) {
        float c2 = cosf(pitch), s2 = sinf(pitch);
        for (int k=0;k<3;k++) out[k] = f[k]*c2 + u[k]*s2;
    } else {
        for (int k=0;k<3;k++) out[k] = f[k];
    }
}

/* Once per frame and view, before any pass: the head centre from the saved
 * pose (position, forward, up), the bearing axes NEAREST measures along, and
 * a fresh scale. */
static inline void halo_panorama_lod_begin(HaloPanoramaLod *lod, int mode, const float pose[9],
                                           const HaloPanoramaView *schedule, int count) {
    lod->mode = mode;
    memcpy(lod->head, pose, sizeof lod->head);
    lod->axes = 0;
    if (mode == HALO_PANORAMA_LOD_NEAREST)
        for (int n = 0; n < count && lod->axes < HALO_PANORAMA_LOD_AXES; n++)
            halo_panorama_lod_bearing_axis(pose + 3, pose + 6, schedule[n].yaw, schedule[n].pitch, lod->axis[lod->axes++]);
    lod->scale = 0.f;
    lod->have_scale = 0;
}

/* A square root in the guest's rounding mode, as FSQRT would give it. */
static inline double halo_panorama_lod_sqrt(const EngineCPU *cpu, double value) {
    if (engine_fp_nearest(cpu)) return sqrt(value);
    static const int modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    int old = fegetround(); fesetround(modes[(cpu->fp_control >> 10) & 3]);
    volatile double result = sqrt(value);
    fesetround(old);
    return result;
}

/* The frame's depth of a point, in the guest's rounding mode. A NaN anywhere
 * stays NaN, which the clamp turns into 0.1 as the original does. */
static inline double halo_panorama_lod_depth(EngineCPU *cpu, const HaloPanoramaLod *lod, const float position[3]) {
    double v[3];
    for (int k = 0; k < 3; k++) v[k] = engine_fp_arithmetic(cpu, ENGINE_FP_SUB, position[k], lod->head[k]);
    if (lod->mode != HALO_PANORAMA_LOD_NEAREST) {
        double sum = engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                         engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                             engine_fp_arithmetic(cpu, ENGINE_FP_MUL, v[0], v[0]),
                             engine_fp_arithmetic(cpu, ENGINE_FP_MUL, v[1], v[1])),
                         engine_fp_arithmetic(cpu, ENGINE_FP_MUL, v[2], v[2]));
        return halo_panorama_lod_sqrt(cpu, sum);
    }
    double best = -INFINITY;
    for (unsigned a = 0; a < lod->axes; a++) {
        const float *axis = lod->axis[a];
        double dot = engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                         engine_fp_arithmetic(cpu, ENGINE_FP_ADD,
                             engine_fp_arithmetic(cpu, ENGINE_FP_MUL, axis[0], v[0]),
                             engine_fp_arithmetic(cpu, ENGINE_FP_MUL, axis[1], v[1])),
                         engine_fp_arithmetic(cpu, ENGINE_FP_MUL, axis[2], v[2]));
        if (isnan(dot)) return dot;
        if (dot > best) best = dot;
    }
    return best;
}

/* 0050F740 natively: EAX = object handle, plain ret, result pushed on the
 * x87 stack. lod NULL or BEARING is the original, planar depth along this
 * view's forward axis; otherwise the frame's depth and scale. The caller has
 * checked that the x87 stack has room for the original's three temporaries. */
static inline void halo_panorama_lod_object_pixels(EngineCPU *cpu, HaloPanoramaLod *lod) {
    uint32_t *r = cpu->gpr;
    uint32_t frame = r[4] - 12u;                                   /* sub esp, 0xc */
    uint32_t globals = engine_read_u32(cpu, 0x006F187Cu);
    uint8_t special = engine_read_u8(cpu, globals + 9u);
    uint32_t objects = engine_read_u32(cpu, 0x008603B0u);
    uint32_t handle = r[0];
    if (special) {
        engine_write_u32(cpu, frame - 4u, r[6]);                  /* push esi / pop esi */
        uint32_t table = engine_read_u32(cpu, objects + 0x34u);
        uint32_t object = engine_read_u32(cpu, table + (handle & 0xFFFFu) * 12u + 8u);
        if (engine_read_u32(cpu, object + 0x10u) & 0x00400000u) {
            engine_fp_push(cpu, engine_read_f32(cpu, 0x00672BE0u)); /* FLT_MAX */
            r[1] = object; r[2] = objects;
            (void)engine_flags_add(&cpu->flags, frame, 12u, 0u, 32u);
            r[4] = frame + 12u;
            cpu->pc = engine_pop(cpu, 4);
            return;
        }
    }
    uint32_t table = engine_read_u32(cpu, objects + 0x34u);
    uint32_t object = engine_read_u32(cpu, table + (handle & 0xFFFFu) * 12u + 8u);
    engine_fp_push(cpu, engine_read_f32(cpu, object + 0xACu));
    uint32_t x = engine_read_u32(cpu, object + 0xA0u);
    uint16_t radius_switch = engine_read_u16(cpu, 0x00689450u);
    uint32_t accumulator = (object & 0xFFFF0000u) | radius_switch;
    engine_write_u32(cpu, frame, x);
    uint32_t y = engine_read_u32(cpu, object + 0xA4u), z = engine_read_u32(cpu, object + 0xA8u);
    engine_write_u32(cpu, frame + 4u, y);
    engine_write_u32(cpu, frame + 8u, z);
    if (radius_switch == 1)
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 0), engine_read_f32(cpu, 0x00672ABCu)));
    else if (radius_switch == 0)
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 0), engine_read_f32(cpu, 0x00672B8Cu)));
    double scale;
    if (!lod || lod->mode == HALO_PANORAMA_LOD_BEARING) {
        engine_fp_push(cpu, engine_read_f32(cpu, 0x007C319Cu));
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 0), engine_read_f32(cpu, frame + 8u)));
        engine_fp_push(cpu, engine_read_f32(cpu, 0x007C3184u));
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 0), engine_read_f32(cpu, frame)));
        engine_fp_write(cpu, 1, engine_fp_arithmetic(cpu, ENGINE_FP_ADD, engine_fp_read(cpu, 1), engine_fp_read(cpu, 0)));
        engine_fp_pop(cpu);
        engine_fp_push(cpu, engine_read_f32(cpu, 0x007C3190u));
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 0), engine_read_f32(cpu, frame + 4u)));
        engine_fp_write(cpu, 1, engine_fp_arithmetic(cpu, ENGINE_FP_ADD, engine_fp_read(cpu, 1), engine_fp_read(cpu, 0)));
        engine_fp_pop(cpu);
        engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_ADD, engine_fp_read(cpu, 0), engine_read_f32(cpu, 0x007C31A8u)));
        scale = engine_read_f32(cpu, 0x007C32F0u);
    } else {
        float position[3];
        memcpy(&position[0], &x, 4); memcpy(&position[1], &y, 4); memcpy(&position[2], &z, 4);
        engine_fp_push(cpu, halo_panorama_lod_depth(cpu, lod, position));
        if (!lod->have_scale) { lod->scale = (float)engine_read_f32(cpu, 0x007C32F0u); lod->have_scale = 1; }
        scale = lod->scale;
    }
    engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_read_f32(cpu, 0x00672AC0u), 0);
    accumulator = (accumulator & 0xFFFF0000u) | engine_fp_status(cpu);
    if ((accumulator >> 8) & 1u) engine_fp_unary(cpu, ENGINE_FP_FCHS);
    engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_read_f32(cpu, 0x00672BACu), 0);
    accumulator = (accumulator & 0xFFFF0000u) | engine_fp_status(cpu);
    if ((accumulator >> 8) & 0x41u) {
        engine_fp_write(cpu, 0, engine_fp_read(cpu, 0));
        engine_fp_pop(cpu);
        engine_fp_push(cpu, engine_read_f32(cpu, 0x00672BACu));
    }
    engine_fp_push(cpu, scale);
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_DIV, engine_fp_read(cpu, 0), engine_fp_read(cpu, 1)));
    engine_fp_write(cpu, 2, engine_fp_arithmetic(cpu, ENGINE_FP_MUL, engine_fp_read(cpu, 2), engine_fp_read(cpu, 0)));
    engine_fp_pop(cpu);
    engine_fp_exchange(cpu, 1);
    engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_ADD, engine_fp_read(cpu, 0), engine_fp_read(cpu, 0)));
    engine_fp_write(cpu, 1, engine_fp_read(cpu, 0));
    engine_fp_pop(cpu);
    r[0] = accumulator; r[1] = y; r[2] = z;
    (void)engine_flags_add(&cpu->flags, frame, 12u, 0u, 32u);   /* add esp, 0xc */
    r[4] = frame + 12u;
    cpu->pc = engine_pop(cpu, 4);
}
#endif
