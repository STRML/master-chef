/* Atmospheric and planar fog measured from the eye rather than along the
 * bearing's own axis, so a point fogs the same in every panorama bearing.
 *
 * Halo keeps fog in two vertex-shader constants, both written once per view
 * by 005176D0 (SetVertexShaderConstantF(6, ..., 4) at 00517A4F):
 *   c6 = s * (fwd, -(near + fwd.eye))   atmospheric fog, s = 1/(far - near)
 *   c8 = a * (fwd, -fwd.eye)            planar fog's viewing depth, a = 1/[007C1448]
 * with fwd the view's forward vector (007C1234) and eye its position
 * (007C1228). 005175C0 calls it and then 00518F40, which uploads c0..c5 with
 * c4 = (eye, 2); 00519F70 rewrites c4 with the same eye. Nothing else writes
 * c4, c6 or c8: the other engine uploads start at c10 or above, and the D3DX
 * effects in fx.bin carry pixel shaders only. The shaders in shaders\vsh.bin
 * evaluate c6 and c8 as planes, dp4 P, c6 (or dp3 P, c6 then add c6.w), so
 * fog grows with depth along fwd. PC uses one forward per view. The panorama
 * draws the same world from nine forwards, so a point fogs by its depth along
 * whichever bearing drew it: equal where two ring bearings meet
 * symmetrically, but not at the joins with the up and down caps, where one
 * bearing sees the point at cos(elevation) of its distance and the cap at
 * sin(elevation). This can produce a brightness step at those joins.
 *
 * The rewrite adds s * (|P - eye| - fwd.(P - eye)) to each such term. The
 * sum is s * (distance - near): the same in every bearing, equal to the
 * original on each bearing's axis. Distance/depth is 1/cos of the angle
 * off-axis; the fog factor also includes near, density and clamping. This
 * yields what PC shows for that point when the player turns to face it.
 * The term's own instruction is kept, so everything the shader does with the
 * value afterwards (density, planar fog blend, oFog, a texture coordinate
 * for the fog-plane pass) is untouched. c7, the fog plane itself, is already
 * a world plane, except when the camera stands in a fog region that has no
 * plane: 00555330 then sets the planar fog type 007C1424 to 2 and 005176D0
 * lays the plane across the view, (fwd, fwd.eye + 007C1268) with 007C144C
 * forced to 1, so c7 = (-fwd, fwd.eye + k) is a depth along fwd too, with
 * the opposite sign. HALO_RADIAL_FOG_VIEW_PLANE rewrites c7 as well for that
 * case. Only instructions that read the constant unswizzled against a
 * position are touched; every read of c6, c7 and c8 in vsh.bin is one of
 * those (tests/test_radial_fog.c checks). s is |cK.xyz| because fwd is a
 * unit vector, so no new constants are needed.
 *
 * The distance is evaluated per vertex and interpolated across the triangle.
 * Every bearing interpolates the same vertex values, so the joins agree
 * within floating-point precision for the same geometry/eye. Inside a large
 * triangle the interpolated distance can exceed the true fragment distance;
 * for an equal-range edge subtending 2*theta the midpoint ratio is 1/cos(theta).
 *
 * Each term T becomes, with t a temporary the shader never uses:
 *     add t.xyz, P, -c4        d = P - eye
 *     dp3 t.w, t, t            d.d
 *     dp3 t.x, t, cK           cK.xyz . d          (s times depth along fwd)
 *     dp3 t.y, cK, cK          s * s
 *     mul t.w, t.w, t.y
 *     rsq t.y, t.w             MojoShader gives FLT_MAX for 0, so a point
 *     mul t.w, t.w, t.y        at the eye yields 0, not NaN
 *     add t.w, t.w, -t.x       s * (|d| - depth)   (-t.w for c7)
 *     T with its destination replaced by t.x
 *     add D, t.x, t.w          D as T wrote it (mask and modifiers)
 * Every instruction reads at most one constant register, as vs_1_1 allows.
 * Shaders with no free temporary are left as they are. */
#ifndef HALO_RADIAL_FOG_H
#define HALO_RADIAL_FOG_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* D3D's true fixed-function vertex fog. Halo's low-capability renderer sets
 * FOGTABLEMODE=NONE and FOGVERTEXMODE=LINEAR in 005176D0. The view transform
 * has already put the eye at the origin, so its vector length is range fog.
 * NONE preserves an application-supplied factor (specular alpha). */
static inline float halo_fixed_radial_fog(const float view[3], unsigned mode,
                                        float start, float end, float density, float supplied) {
    float distance = sqrtf(view[0]*view[0] + view[1]*view[1] + view[2]*view[2]);
    float factor = supplied;
    if (mode == 1) factor = expf(-density * distance);
    else if (mode == 2) { float d = density * distance; factor = expf(-d*d); }
    else if (mode == 3) factor = end != start ? (end - distance) / (end - start) : (distance < end ? 1.f : 0.f);
    return fminf(1.f, fmaxf(0.f, factor));
}

enum { HALO_RADIAL_FOG_EYE = 4, HALO_RADIAL_FOG_ATMOSPHERE = 6, HALO_RADIAL_FOG_PLANE = 7, HALO_RADIAL_FOG_PLANAR_DEPTH = 8 };
/* Which constants to measure from the eye: c6 and c8 always; c7 as well
 * while the planar fog type is 2 (the plane lies across the view). */
enum { HALO_RADIAL_FOG_DEPTH = 1, HALO_RADIAL_FOG_VIEW_PLANE = 2 };
/* Tokens one rewritten term adds: ten instructions (39 tokens) take the
 * place of the original one (4). */
#define HALO_RADIAL_FOG_GROWTH 35u

static inline unsigned halo_rf_type(uint32_t token) { return ((token >> 28) & 7u) | ((token >> 8) & 24u); }
static inline unsigned halo_rf_number(uint32_t token) { return token & 2047u; }
static inline uint32_t halo_rf_dst(unsigned type, unsigned number, unsigned mask) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 24u) << 8) | number | (mask << 16);
}
static inline uint32_t halo_rf_src(unsigned type, unsigned number, unsigned swizzle, unsigned negate) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 24u) << 8) | number | (swizzle << 16) | (negate ? 1u << 24 : 0u);
}
/* Operand tokens after the instruction token for vs_1_1, which carries no
 * length field. -1 for anything this rewrite does not know how to step over. */
static inline int halo_rf_operands(unsigned op) {
    switch (op) {
        case 0: return 0;
        case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: return 2;
        case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17: return 3;
        /* Matrix instructions read implicit register spans, so the explicit
         * operand scan cannot prove a scratch register is unused. None of
         * Halo's 64 shaders uses them; preserve custom shaders unchanged. */
        case 4: case 18: return 4;
        case 31: return 2;
        case 81: return 5;
        default: return -1;
    }
}
/* A plain, unswizzled, unmodified read of a temporary or input. */
static inline int halo_rf_position(uint32_t src) {
    unsigned type = halo_rf_type(src), n = halo_rf_number(src);
    return (src & 0x80000000u) && ((type == 0 && n < 12) || (type == 1 && n < 16)) &&
           ((src >> 16) & 255u) == 0xE4u && !((src >> 24) & 15u) && !(src & 0x2000u);
}
static inline int halo_rf_fog_constant(uint32_t src, int mode) {
    unsigned n = halo_rf_number(src);
    int wanted = n == HALO_RADIAL_FOG_ATMOSPHERE || n == HALO_RADIAL_FOG_PLANAR_DEPTH ||
                 (n == HALO_RADIAL_FOG_PLANE && mode == HALO_RADIAL_FOG_VIEW_PLANE);
    return (src & 0x80000000u) && halo_rf_type(src) == 2 && wanted && ((src >> 16) & 255u) == 0xE4u && !((src >> 24) & 15u) && !(src & 0x2000u);
}
/* dp3 P, cK is a plane only when the next instruction adds cK.w to every
 * lane it wrote, as vs25 and vs28 do; any other dp3 of c6/c8 is left alone. */
static inline int halo_rf_plane_w(const uint32_t *dp3, const uint32_t *next) {
    if ((next[0] & 0xFFFFu) != 2 || (next[0] & 0xFFFF0000u)) return 0;
    uint32_t d = dp3[1], nd = next[1], a = next[2], b = next[3], k = dp3[3];
    if (nd != d || halo_rf_type(d) != 0) return 0;
    unsigned mask = (d >> 16) & 15u;
    for (int order = 0; order < 2; ++order) {
        uint32_t same = order ? b : a, w = order ? a : b;
        if (halo_rf_type(same) != 0 || halo_rf_number(same) != halo_rf_number(d) || ((same >> 24) & 15u) || (same & 0x2000u)) continue;
        if (halo_rf_type(w) != 2 || halo_rf_number(w) != halo_rf_number(k) || ((w >> 24) & 15u) || (w & 0x2000u)) continue;
        int ok = 1;
        for (unsigned lane = 0; lane < 4; ++lane) if (mask & (1u << lane))
            ok &= ((same >> (16 + 2 * lane)) & 3u) == lane && ((w >> (16 + 2 * lane)) & 3u) == 3u;
        if (ok) return 1;
    }
    return 0;
}
/* Is the instruction at w (with n operand tokens) a fog term to rewrite? */
static inline int halo_rf_term(const uint32_t *w, int n, const uint32_t *end, int mode) {
    unsigned op = w[0] & 0xFFFFu;
    if ((op != 8 && op != 9) || n != 3 || (w[0] & 0xFFFF0000u)) return 0;
    if (!halo_rf_position(w[2]) || !halo_rf_fog_constant(w[3], mode)) return 0;
    if (op == 9) return 1;
    /* A clamp/shift between dp3 and the .w add breaks the affine plane. */
    if (w[1] & 0x0FF00000u) return 0;
    return end - w >= 8 && halo_rf_plane_w(w, w + 4);
}
/* Walks a vs_1_1 token stream: the number of fog terms, and in *temp the
 * highest temporary the shader never touches. 0 when there is nothing to
 * rewrite, an instruction it cannot step over, or no free temporary. */
static inline int halo_rf_scan(const uint32_t *tokens, size_t bytes, int mode, int *temp) {
    if (temp) *temp = -1;
    if (mode != HALO_RADIAL_FOG_DEPTH && mode != HALO_RADIAL_FOG_VIEW_PLANE) return 0;
    if (!tokens || bytes < 8 || bytes > 1024 * 1024 || (bytes & 3u) || tokens[0] != 0xFFFE0101u) return 0;
    const size_t count = bytes / 4;
    const uint32_t *end = tokens + count;
    unsigned temps = 0, terms = 0, ended = 0;
    for (size_t i = 1; i < count;) {
        uint32_t token = tokens[i]; unsigned op = token & 0xFFFFu;
        if (op == 0xFFFFu) { ended = 1; break; }
        if (op == 0xFFFEu) { size_t n = (token >> 16) & 0x7FFFu; if (n > count - i - 1) return 0; i += 1 + n; continue; }
        int n = halo_rf_operands(op);
        if (n < 0 || (token & 0xFFFF0000u) || (size_t)n > count - i - 1) return 0;
        if (op != 31 && op != 81)
            for (int k = 1; k <= n; ++k) {
                uint32_t operand = tokens[i + k];
                if (halo_rf_type(operand) == 0) { unsigned r = halo_rf_number(operand); if (r >= 12) return 0; temps |= 1u << r; }
            }
        if (halo_rf_term(tokens + i, n, end, mode)) terms++;
        i += 1 + (size_t)n;
    }
    if (!ended || !terms) return 0;
    for (int r = 11; r >= 0; --r) if (!(temps & (1u << r))) { if (temp) *temp = r; return (int)terms; }
    return 0;
}
/* How many terms halo_radial_fog_rewrite would change, without allocating. */
static inline int halo_radial_fog_terms(const uint32_t *tokens, size_t bytes, int mode) {
    return halo_rf_scan(tokens, bytes, mode, NULL);
}

/* Returns the number of fog terms rewritten. 0 leaves *out NULL: nothing to
 * change, an unknown instruction, or no free temporary. -1 only for an
 * allocation failure. On success *out is malloc'd and the caller frees it. */
static inline int halo_radial_fog_rewrite(const uint32_t *tokens, size_t bytes, int mode, uint32_t **out, size_t *out_bytes) {
    if (out) *out = NULL;
    if (out_bytes) *out_bytes = 0;
    if (!out || !out_bytes) return 0;
    int t = -1, terms = halo_rf_scan(tokens, bytes, mode, &t);
    if (terms <= 0 || t < 0) return 0;
    const size_t count = bytes / 4;
    const uint32_t *end = tokens + count;
    size_t capacity = count + (size_t)terms * HALO_RADIAL_FOG_GROWTH;
    uint32_t *o = malloc(capacity * 4);
    if (!o) return -1;
    size_t at = 0;
    o[at++] = tokens[0];
    const unsigned T = (unsigned)t, X = 0x00u, Y = 0x55u, W = 0xFFu, XYZW = 0xE4u;
#define HALO_RF_EMIT3(op, d, a, b) do { o[at++] = (op); o[at++] = (d); o[at++] = (a); o[at++] = (b); } while (0)
    for (size_t i = 1; i < count;) {
        uint32_t token = tokens[i]; unsigned op = token & 0xFFFFu;
        if (op == 0xFFFFu) { o[at++] = token; break; }
        size_t n = op == 0xFFFEu ? (token >> 16) & 0x7FFFu : (size_t)halo_rf_operands(op);
        if (op != 0xFFFEu && halo_rf_term(tokens + i, (int)n, end, mode)) {
            uint32_t position = tokens[i + 2], k = tokens[i + 3];
            unsigned fog = halo_rf_number(k), away = fog == HALO_RADIAL_FOG_PLANE; /* c7 = (-fwd, ...) */
            HALO_RF_EMIT3(2u, halo_rf_dst(0, T, 7), position, halo_rf_src(2, HALO_RADIAL_FOG_EYE, XYZW, 1));
            HALO_RF_EMIT3(8u, halo_rf_dst(0, T, 8), halo_rf_src(0, T, XYZW, 0), halo_rf_src(0, T, XYZW, 0));
            HALO_RF_EMIT3(8u, halo_rf_dst(0, T, 1), halo_rf_src(0, T, XYZW, 0), halo_rf_src(2, fog, XYZW, 0));
            HALO_RF_EMIT3(8u, halo_rf_dst(0, T, 2), halo_rf_src(2, fog, XYZW, 0), halo_rf_src(2, fog, XYZW, 0));
            HALO_RF_EMIT3(5u, halo_rf_dst(0, T, 8), halo_rf_src(0, T, W, 0), halo_rf_src(0, T, Y, 0));
            o[at++] = 7u; o[at++] = halo_rf_dst(0, T, 2); o[at++] = halo_rf_src(0, T, W, 0);
            HALO_RF_EMIT3(5u, halo_rf_dst(0, T, 8), halo_rf_src(0, T, W, 0), halo_rf_src(0, T, Y, 0));
            HALO_RF_EMIT3(2u, halo_rf_dst(0, T, 8), halo_rf_src(0, T, W, away), halo_rf_src(0, T, X, 1));
            HALO_RF_EMIT3(token, halo_rf_dst(0, T, 1), position, k);
            HALO_RF_EMIT3(2u, tokens[i + 1], halo_rf_src(0, T, X, 0), halo_rf_src(0, T, W, 0));
        } else {
            memcpy(o + at, tokens + i, (1 + n) * 4); at += 1 + n;
        }
        i += 1 + n;
    }
#undef HALO_RF_EMIT3
    *out = o; *out_bytes = at * 4;
    return terms;
}
#endif
