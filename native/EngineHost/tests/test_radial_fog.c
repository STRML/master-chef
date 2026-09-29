/* Radial fog (radial_fog.h): Halo's fog measured from the eye, so every
 * panorama bearing fogs a world point alike.
 *
 * The rewrite changes the vertex shaders, not guest code, so the reference
 * is the original shader run with fog constants aimed straight at the vertex:
 * the fog PC shows for that point when the player faces it. An interpreter
 * with MojoShader's Metal semantics (rsq and rcp of 0 give FLT_MAX) runs the
 * original and the rewritten shader for nine bearings (six round the ring,
 * straight up and down, and an arbitrary head direction) and checks
 *   - every output that does not depend on c6/c8 (or c7 in type 2) is bit
 *     identical to the original's,
 *   - every output equals the original with the vertex-aimed fog constants,
 *   - an output that depends on the bearing only through fog is the same in
 *     every bearing (the original differs, which is the seam),
 *   - on a bearing's own axis and at the eye nothing changes.
 * Parts:
 *   1. Synthetic vs_1_1 shaders in every form Halo uses (dp4 of a position,
 *      dp3 plus .w, oFog, fog-plane texture coordinates, skinned with a0),
 *      the camera-in-planeless-fog case, and malformed or unsuitable input.
 *   2. With the generated engine and the .rdata image present, the view
 *      constants come from the translated 005176D0 and 00518F40 themselves
 *      (every SetVertexShaderConstantF they make is captured), for all three
 *      planar fog types, and the model the test uses elsewhere is checked
 *      against them.
 *   3. With Halo's shaders\vsh.bin present, all 64 shaders: every read of
 *      c6, c7 and c8 is a term the rewrite recognises, and the checks above.
 *   4. MojoShader translates every rewritten shader, binds c4, keeps the same
 *      outputs and guards rsq(0).
 * Parts 2 and 3 print SKIP when their inputs are absent. Also times the
 * rewrite (once per pipeline) and reports the instruction growth.
 *
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -DMOJOSHADER_NO_VERSION_INCLUDE=1 <MOJO defines>
 *   -I native/EngineHost -I native/EngineReuse -I third_party/mojoshader
 *   native/EngineHost/tests/test_radial_fog.c native/EngineHost/metalshader.c
 *   third_party/mojoshader/{mojoshader,mojoshader_common}.c
 *   third_party/mojoshader/profiles/mojoshader_profile_{common,metal}.c -lm
 */
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 1   /* the release setting */
#endif
#pragma STDC FP_CONTRACT OFF
#include "engine_cpu.h"
#include "radial_fog.h"
#include "metalshader.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* Source-only checks need no proprietary inputs. Enable the extra constant
 * upload reference by compiling with -DRF_HAVE_ENGINE=1 -I <generated-dir>.
 * tools/run_source_checks.py does that when HALO_RADIAL_FOG_GENERATED_DIR is
 * set. Runtime fixture paths are HALO_RADIAL_FOG_RDATA and HALO_RADIAL_FOG_VSH;
 * absent overrides, only repository-relative locations are consulted. */
#ifndef RF_HAVE_ENGINE
#define RF_HAVE_ENGINE 0
#endif
#if RF_HAVE_ENGINE
#include "engine_functions.h"
#endif
static const char *rf_fixture_path(const char *variable, const char *fallback) {
    const char *path = getenv(variable);
    return path && *path ? path : fallback;
}

uint8_t *engine_flat_base;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t rng = 0x243F6A8885A308D3ull;
static double rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (double)(rng >> 11) * 0x1.0p-53; }
static double rnd_range(double a, double b) { return a + (b - a) * rnd(); }
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; if (failures < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* ---------- a vs_1_1 interpreter with MojoShader's Metal semantics ---------- */

enum { O_POS, O_FOG, O_PTS, O_D0, O_D1, O_T0, O_COUNT = O_T0 + 8 };
typedef struct {
    float c[256][4]; uint8_t fog_taint[256];   /* constants; fog_taint marks c6/c8 (and c7 in type 2) */
    float in[16][4];
} RFEnv;
typedef struct {
    float o[O_COUNT][4]; uint8_t taint[O_COUNT][4]; uint8_t written[O_COUNT];
    float tap[3]; int taps, tap_mismatch;       /* the position read by the fog terms */
    unsigned instructions;
} RFOut;

static int rf_src(const float r[12][4], const uint8_t rt[12][4], const RFEnv *e, int a0, uint8_t a0t,
                  uint32_t tok, float v[4], uint8_t t[4]) {
    unsigned type = halo_rf_type(tok), n = halo_rf_number(tok), mod = (tok >> 24) & 15u;
    const float *reg; uint8_t base = 0; const uint8_t *regt = NULL; static const uint8_t none[4];
    if (mod > 1) return 0;
    if (type == 0) { if (n >= 12) return 0; reg = r[n]; regt = rt[n]; }
    else if (type == 1) { if (n >= 16) return 0; reg = e->in[n]; regt = none; }
    else if (type == 2) {
        int index = (int)n;
        if (tok & 0x2000u) { index += a0; base = a0t; }
        if (index < 0 || index >= 256) return 0;
        reg = e->c[index]; base |= e->fog_taint[index]; regt = none;
    } else return 0;
    if ((tok & 0x2000u) && type != 2) return 0;
    for (unsigned c = 0; c < 4; ++c) {
        unsigned s = (tok >> (16 + 2 * c)) & 3u;
        v[c] = mod ? -reg[s] : reg[s]; t[c] = regt[s] | base;
    }
    return 1;
}
static int rf_run(const uint32_t *tokens, size_t bytes, const RFEnv *e, RFOut *out) {
    memset(out, 0, sizeof *out);
    if (bytes < 8 || tokens[0] != 0xFFFE0101u) return 0;
    float r[12][4] = {{0}}; uint8_t rt[12][4] = {{0}}; int a0 = 0; uint8_t a0t = 0;
    size_t count = bytes / 4;
    for (size_t i = 1; i < count;) {
        uint32_t tok = tokens[i]; unsigned op = tok & 0xFFFFu;
        if (op == 0xFFFFu) return 1;
        if (op == 0xFFFEu) { i += 1 + ((tok >> 16) & 0x7FFFu); continue; }
        int n = halo_rf_operands(op);
        if (n < 0 || i + 1 + (size_t)n > count) return 0;
        const uint32_t *w = tokens + i;
        i += 1 + (size_t)n;
        if (op == 31 || op == 0) continue;
        if (op == 81) return 0;                       /* Halo's shaders carry no def */
        if (halo_rf_term(w, n, tokens + count, HALO_RADIAL_FOG_VIEW_PLANE)) {
            float p[4]; uint8_t pt[4];
            if (!rf_src(r, rt, e, a0, a0t, w[2], p, pt)) return 0;
            if (!out->taps) memcpy(out->tap, p, sizeof out->tap);
            else if (memcmp(out->tap, p, sizeof out->tap)) out->tap_mismatch = 1;
            out->taps++;
        }
        float a[3][4] = {{0}}, res[4] = {0}; uint8_t at[3][4] = {{0}}, restaint[4] = {0};
        for (int k = 0; k < n - 1; ++k) if (!rf_src(r, rt, e, a0, a0t, w[2 + k], a[k], at[k])) return 0;
        out->instructions++;
        switch (op) {
            case 1: for (int c = 0; c < 4; ++c) { res[c] = a[0][c]; restaint[c] = at[0][c]; } break;
            case 2: for (int c = 0; c < 4; ++c) { res[c] = a[0][c] + a[1][c]; restaint[c] = at[0][c] | at[1][c]; } break;
            case 3: for (int c = 0; c < 4; ++c) { res[c] = a[0][c] - a[1][c]; restaint[c] = at[0][c] | at[1][c]; } break;
            case 4: for (int c = 0; c < 4; ++c) { res[c] = (a[0][c] * a[1][c]) + a[2][c]; restaint[c] = at[0][c] | at[1][c] | at[2][c]; } break;
            case 5: for (int c = 0; c < 4; ++c) { res[c] = a[0][c] * a[1][c]; restaint[c] = at[0][c] | at[1][c]; } break;
            case 6: case 7: {
                float x = a[0][0];
                float y = x == 0.0f ? FLT_MAX : op == 6 ? 1.0f / x : 1.0f / sqrtf(fabsf(x));
                for (int c = 0; c < 4; ++c) { res[c] = y; restaint[c] = at[0][0]; }
            } break;
            case 8: case 9: {
                float d = 0; uint8_t dt = 0;
                for (int c = 0; c < (op == 8 ? 3 : 4); ++c) { d = d + a[0][c] * a[1][c]; dt |= at[0][c] | at[1][c]; }
                for (int c = 0; c < 4; ++c) { res[c] = d; restaint[c] = dt; }
            } break;
            case 10: for (int c = 0; c < 4; ++c) { res[c] = fminf(a[0][c], a[1][c]); restaint[c] = at[0][c] | at[1][c]; } break;
            case 11: for (int c = 0; c < 4; ++c) { res[c] = fmaxf(a[0][c], a[1][c]); restaint[c] = at[0][c] | at[1][c]; } break;
            case 12: for (int c = 0; c < 4; ++c) { res[c] = a[0][c] < a[1][c] ? 1.f : 0.f; restaint[c] = at[0][c] | at[1][c]; } break;
            case 13: for (int c = 0; c < 4; ++c) { res[c] = a[0][c] >= a[1][c] ? 1.f : 0.f; restaint[c] = at[0][c] | at[1][c]; } break;
            default: printf("interpreter: opcode %u unsupported\n", op); return 0;
        }
        uint32_t d = w[1]; unsigned type = halo_rf_type(d), num = halo_rf_number(d), mask = (d >> 16) & 15u;
        unsigned modifier = (d >> 20) & 15u;
        if ((d >> 24) & 15u) return 0;
        if (modifier & 1u) for (int c = 0; c < 4; ++c) res[c] = fminf(1.f, fmaxf(0.f, res[c]));
        float *dst; uint8_t *dstt; int slot = -1;
        if (type == 0 && num < 12) { dst = r[num]; dstt = rt[num]; }
        else if (type == 3 && num == 0) { if (mask & 1u) { a0 = (int)floorf(res[0]); a0t = restaint[0]; } continue; }
        else if (type == 4 && num < 3) slot = num == 0 ? O_POS : num == 1 ? O_FOG : O_PTS;
        else if (type == 5 && num < 2) slot = O_D0 + (int)num;
        else if (type == 6 && num < 8) slot = O_T0 + (int)num;
        else return 0;
        if (slot >= 0) { dst = out->o[slot]; dstt = out->taint[slot]; out->written[slot] |= (uint8_t)mask; }
        for (int c = 0; c < 4; ++c) if (mask & (1u << c)) { dst[c] = res[c]; dstt[c] = restaint[c]; }
    }
    return 0;   /* no end token */
}

/* ---------- the view constants, as 005176D0 and 00518F40 upload them ---------- */

typedef struct { double near_, far_, density; int type; double plane[4], planar_density, a, b, k; } RFFog;
typedef struct { double eye[3], fwd[3]; } RFView;
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void rf_view_axes(const double f[3], double right[3], double up[3]) {
    double z[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 };
    const double *h = fabs(f[2]) > 0.99 ? x : z;
    right[0] = f[1] * h[2] - f[2] * h[1]; right[1] = f[2] * h[0] - f[0] * h[2]; right[2] = f[0] * h[1] - f[1] * h[0];
    double l = sqrt(dot3(right, right)); for (int i = 0; i < 3; ++i) right[i] /= l;
    up[0] = right[1] * f[2] - right[2] * f[1]; up[1] = right[2] * f[0] - right[0] * f[2]; up[2] = right[0] * f[1] - right[1] * f[0];
}
/* c0..c9 for one view. c0..c3 is any per-view projection (only oPos reads
 * it); c4..c9 follow 00518F40 and 005176D0 (see radial_fog.h). */
static void rf_constants(RFEnv *e, const RFView *v, const RFFog *f) {
    double right[3], up[3], n[3], d, a = 1, b = 1, planar = 0;
    rf_view_axes(v->fwd, right, up);
    const double *rows[4] = { right, up, v->fwd, v->fwd };
    for (int r = 0; r < 4; ++r) {
        double scale = r == 2 ? 1.0001 : 1;
        for (int i = 0; i < 3; ++i) e->c[r][i] = (float)(rows[r][i] * scale);
        e->c[r][3] = (float)(-dot3(rows[r], v->eye) * scale - (r == 2 ? 0.0625 : 0));
    }
    if (f->type == 1) { memcpy(n, f->plane, sizeof n); d = f->plane[3]; a = f->a; b = f->b; planar = f->planar_density; }
    else if (f->type == 2) { memcpy(n, v->fwd, sizeof n); d = dot3(v->fwd, v->eye) + f->k; a = f->a; b = 1; planar = f->planar_density; }
    else { memcpy(n, v->fwd, sizeof n); d = dot3(v->fwd, v->eye); }
    double s = 1.0 / (f->far_ - f->near_), fe = dot3(v->fwd, v->eye);
    for (int i = 0; i < 3; ++i) {
        e->c[4][i] = (float)v->eye[i]; e->c[5][i] = (float)v->fwd[i];
        e->c[6][i] = (float)(v->fwd[i] * s); e->c[7][i] = (float)-(n[i] / b); e->c[8][i] = (float)(v->fwd[i] / a);
    }
    e->c[4][3] = 2; e->c[5][3] = 0.5f;
    e->c[6][3] = (float)-((f->near_ + fe) * s); e->c[7][3] = (float)(d / b); e->c[8][3] = (float)-(fe / a);
    e->c[9][0] = (float)clamp01(f->density); e->c[9][1] = (float)clamp01(-(dot3(v->eye, n) - d) / b);
    e->c[9][2] = (float)clamp01(planar); e->c[9][3] = 3;
    memset(e->fog_taint, 0, sizeof e->fog_taint);
    e->fog_taint[6] = e->fog_taint[8] = 1; e->fog_taint[7] = f->type == 2;
}
static int rf_mode(const RFFog *f) { return f->type == 2 ? HALO_RADIAL_FOG_VIEW_PLANE : HALO_RADIAL_FOG_DEPTH; }

/* The bearings of one frame: the ring, the caps, and a head direction. */
enum { BEARINGS = 9 };
static void rf_bearing(int i, double fwd[3]) {
    static const double yaw[] = { 0, -60, 60, 120, 180, -120 };
    if (i < 6) { double y = yaw[i] * M_PI / 180; fwd[0] = cos(y); fwd[1] = sin(y); fwd[2] = 0; }
    else if (i == 6) { fwd[0] = 0; fwd[1] = 0; fwd[2] = 1; }
    else if (i == 7) { fwd[0] = 0; fwd[1] = 0; fwd[2] = -1; }
    else { double y = 23 * M_PI / 180, p = 41 * M_PI / 180; fwd[0] = cos(p) * cos(y); fwd[1] = cos(p) * sin(y); fwd[2] = sin(p); }
}

/* ---------- the differential over one shader ---------- */

typedef struct { unsigned vertices, rewritten_outputs, independent, skipped; double worst_reference, worst_spread, original_spread; } RFStats;
static float rf_tolerance(float reference) { return 2e-3f + 2e-4f * fabsf(reference); }
/* Constants besides the view's: texture transforms and node matrices. */
static void rf_fill_constants(RFEnv *e, uint64_t seed) {
    uint64_t saved = rng; rng = seed;
    for (int r = 10; r < 256; ++r) for (int c = 0; c < 4; ++c) e->c[r][c] = (float)rnd_range(-1, 1);
    for (int r = 26; r < 29; ++r) { e->c[r][0] = e->c[r][1] = e->c[r][2] = 0; e->c[r][r - 26] = 1; e->c[r][3] = (float)rnd_range(-3, 3); }
    rng = saved;
}
static void rf_inputs(RFEnv *e, const double p[3]) {
    for (int r = 0; r < 16; ++r) for (int c = 0; c < 4; ++c) e->in[r][c] = (float)rnd_range(-1, 1);
    for (int c = 0; c < 3; ++c) e->in[0][c] = (float)p[c];
    e->in[0][3] = 1;
    e->in[4][2] = 0; e->in[4][3] = 1;   /* Halo reads v4.z and v4.w as 0 and 1 */
    e->in[5][0] = (float)(int)rnd_range(0, 20); e->in[5][1] = (float)(int)rnd_range(0, 20);
    double w = rnd(); e->in[6][0] = (float)w; e->in[6][1] = (float)(1 - w);
    for (int c = 0; c < 4; ++c) e->in[9][c] = (float)rnd();
}
static int rf_output_equal(const RFOut *a, const RFOut *b, int slot, int c) {
    uint32_t x, y; memcpy(&x, &a->o[slot][c], 4); memcpy(&y, &b->o[slot][c], 4); return x == y;
}
/* Constants for view v with only the fog constants taken from view u. */
static void rf_mix(RFEnv *out, const RFEnv *v, const RFEnv *u) {
    *out = *v;
    for (int r = 6; r <= 8; ++r) if (u->fog_taint[r]) memcpy(out->c[r], u->c[r], sizeof out->c[r]);
}
typedef void (*RFConstants)(RFEnv *, const RFView *, const RFFog *);

static void rf_differential(const char *name, const uint32_t *tokens, size_t bytes, const RFFog *fog,
                            unsigned vertices, RFConstants constants, RFStats *st) {
    int mode = rf_mode(fog);
    uint32_t *rw = NULL; size_t rwb = 0;
    int terms = halo_radial_fog_rewrite(tokens, bytes, mode, &rw, &rwb);
    if (terms <= 0) { st->skipped++; return; }
    RFEnv base; memset(&base, 0, sizeof base); rf_fill_constants(&base, 0x9E3779B97F4A7C15ull ^ bytes);
    for (unsigned n = 0; n < vertices; ++n) {
        double eye[3], p[3], dir[3];
        for (int i = 0; i < 3; ++i) eye[i] = rnd_range(-150, 150);
        /* Directions over the whole sphere, a third of them in the band
         * where the ring bearings hand over to the caps (42..57 degrees). */
        double az = rnd_range(-M_PI, M_PI), el = (n % 3 == 0) ? (rnd() < 0.5 ? -1 : 1) * rnd_range(42, 57) * M_PI / 180
                                                             : asin(rnd_range(-1, 1));
        double dist = n % 17 == 0 ? 0 : exp(rnd_range(log(0.05), log(400)));
        dir[0] = cos(el) * cos(az); dir[1] = cos(el) * sin(az); dir[2] = sin(el);
        for (int i = 0; i < 3; ++i) p[i] = eye[i] + dir[i] * dist;
        RFEnv e = base; rf_inputs(&e, p);
        RFOut orig[BEARINGS], rew[BEARINGS], ref[BEARINGS], axis[BEARINGS];
        int ran = 1, comparable = 1;
        for (int b = 0; b < BEARINGS && ran; ++b) {
            RFView view; memcpy(view.eye, eye, sizeof eye); rf_bearing(b, view.fwd);
            RFEnv env = e; constants(&env, &view, fog);
            ran = rf_run(tokens, bytes, &env, &orig[b]) && rf_run(rw, rwb, &env, &rew[b]);
            if (!ran) break;
            if (!orig[b].taps || orig[b].tap_mismatch) { comparable = 0; break; }
            /* The fog constants of a view aimed at the position the terms
             * read, which is this bearing's: a billboard's corners turn with
             * the view (vs04 builds them from c5), so each bearing has its own. */
            RFView aimed; memcpy(aimed.eye, eye, sizeof eye);
            double to[3], len = 0;
            for (int i = 0; i < 3; ++i) { to[i] = orig[b].tap[i] - eye[i]; len += to[i] * to[i]; }
            len = sqrt(len);
            if (len > 1e-3) for (int i = 0; i < 3; ++i) aimed.fwd[i] = to[i] / len;
            else memcpy(aimed.fwd, view.fwd, sizeof aimed.fwd);
            RFEnv aimed_env = e, mixed; constants(&aimed_env, &aimed, fog); rf_mix(&mixed, &env, &aimed_env);
            /* ref: PC's fog for the point, facing it. axis: the rewrite with
             * that same view, which must not change anything. */
            ran = rf_run(tokens, bytes, &mixed, &ref[b]) && rf_run(rw, rwb, &mixed, &axis[b]);
        }
        CHECK(ran, "%s: a run failed", name);
        if (!ran || !comparable) { st->skipped++; continue; }
        st->vertices++;
        for (int slot = 0; slot < O_COUNT; ++slot) for (int c = 0; c < 4; ++c) {
            if (!(orig[0].written[slot] & (1u << c))) continue;
            CHECK(rew[0].written[slot] & (1u << c), "%s: output %d.%d no longer written", name, slot, c);
            int fogged = 0; float lo = FLT_MAX, hi = -FLT_MAX, olo = FLT_MAX, ohi = -FLT_MAX, rlo = FLT_MAX, rhi = -FLT_MAX;
            for (int b = 0; b < BEARINGS; ++b) {
                float got = rew[b].o[slot][c], want = ref[b].o[slot][c];
                CHECK(isfinite(got) == isfinite(want), "%s: output %d.%d finite mismatch", name, slot, c);
                if (!orig[b].taint[slot][c])
                    CHECK(rf_output_equal(&orig[b], &rew[b], slot, c), "%s: output %d.%d changed without fog (%a vs %a)",
                          name, slot, c, orig[b].o[slot][c], rew[b].o[slot][c]);
                else fogged = 1;
                double diff = fabs((double)got - want);
                CHECK(diff <= rf_tolerance(want), "%s: output %d.%d bearing %d: %.7g, PC facing the point %.7g",
                      name, slot, c, b, got, want);
                if (diff / (1 + fabs(want)) > st->worst_reference) st->worst_reference = diff / (1 + fabs(want));
                CHECK(fabsf(axis[b].o[slot][c] - want) <= rf_tolerance(want), "%s: on-axis output %d.%d moved %.7g -> %.7g",
                      name, slot, c, want, axis[b].o[slot][c]);
                lo = fminf(lo, got); hi = fmaxf(hi, got); olo = fminf(olo, orig[b].o[slot][c]); ohi = fmaxf(ohi, orig[b].o[slot][c]);
                rlo = fminf(rlo, want); rhi = fmaxf(rhi, want);
            }
            if (!fogged) continue;
            st->rewritten_outputs++;
            /* Bearing-independent apart from fog: PC facing the point gives
             * the same value whichever bearing's other constants it has. */
            if (rhi - rlo <= 1e-4f * (1 + fabsf(rhi))) {   /* beyond the engine's own float rounding */
                st->independent++;
                CHECK(hi - lo <= rf_tolerance(hi), "%s: output %d.%d differs across bearings by %.3g", name, slot, c, hi - lo);
                if ((hi - lo) / (1 + fabsf(hi)) > st->worst_spread) st->worst_spread = (hi - lo) / (1 + fabsf(hi));
                if (ohi - olo > st->original_spread) st->original_spread = ohi - olo;
            }
        }
    }
    free(rw);
}

/* ---------- synthetic shaders ---------- */

typedef struct { uint32_t t[512]; size_t n; } RFShader;
static void emit(RFShader *s, uint32_t v) { assert(s->n < 512); s->t[s->n++] = v; }
static void ins(RFShader *s, unsigned op, int count, ...) {
    va_list ap; va_start(ap, count); emit(s, op); for (int i = 0; i < count; ++i) emit(s, va_arg(ap, uint32_t)); va_end(ap);
}
enum { TR = 0, TV = 1, TC = 2, TA = 3, TRAST = 4, TATTR = 5, TTEX = 6 };
#define XYZW 0xE4u
#define XXXX 0x00u
#define YYYY 0x55u
#define ZZZZ 0xAAu
#define WWWW 0xFFu
#define XXXY 0x40u
static uint32_t D(unsigned type, unsigned n, unsigned mask) { return halo_rf_dst(type, n, mask); }
static uint32_t S(unsigned type, unsigned n, unsigned swz) { return halo_rf_src(type, n, swz, 0); }
static uint32_t NS(unsigned type, unsigned n, unsigned swz) { return halo_rf_src(type, n, swz, 1); }
static uint32_t REL(unsigned n) { return halo_rf_src(TC, n, XYZW, 0) | 0x2000u; }
static void begin(RFShader *s, int skinned) {
    s->n = 0; emit(s, 0xFFFE0101u);
    ins(s, 31, 2, 0x80000000u, D(TV, 0, 15));            /* dcl_position v0 */
    ins(s, 31, 2, 0x80000003u, D(TV, 1, 15));            /* dcl_normal v1 */
    ins(s, 31, 2, 0x80000005u, D(TV, 4, 15));            /* dcl_texcoord v4 */
    if (skinned) { ins(s, 31, 2, 0x80000002u, D(TV, 5, 15)); ins(s, 31, 2, 0x80000001u, D(TV, 6, 15)); }
    ins(s, 0xFFFEu | (2u << 16), 2, 0x43544C48u, 0u);  /* a comment the rewrite must step over */
}
static void project(RFShader *s, unsigned p_type, unsigned p) {
    for (unsigned r = 0; r < 4; ++r) ins(s, 9, 3, D(TRAST, 0, 1u << r), S(p_type, p, XYZW), S(TC, r, XYZW));
}
static void node(RFShader *s) {   /* r0 = world position through the node matrix c26..c28 */
    for (unsigned r = 0; r < 3; ++r) ins(s, 9, 3, D(TR, 0, 1u << r), S(TV, 0, XYZW), S(TC, 26 + r, XYZW));
    ins(s, 1, 2, D(TR, 0, 8), S(TV, 4, WWWW));
}
/* vs37's atmospheric and planar fog chain on position p, into oD0.w, plus
 * its view-angle term |N.fwd| (c5) into oD1. */
static void fog_chain(RFShader *s, unsigned p_type, unsigned p) {
    ins(s, 9, 3, D(TR, 8, 1), S(p_type, p, XYZW), S(TC, 7, XYZW));
    ins(s, 9, 3, D(TR, 8, 2), S(p_type, p, XYZW), S(TC, 8, XYZW));
    ins(s, 9, 3, D(TR, 8, 4), S(p_type, p, XYZW), S(TC, 6, XYZW));
    ins(s, 2, 3, D(TR, 8, 3), S(TV, 4, WWWW), NS(TR, 8, XYZW));
    ins(s, 11, 3, D(TR, 8, 7), S(TR, 8, XYZW), S(TV, 4, ZZZZ));
    ins(s, 5, 3, D(TR, 8, 3), S(TR, 8, XYZW), S(TR, 8, XYZW));
    ins(s, 10, 3, D(TR, 8, 7), S(TR, 8, XYZW), S(TV, 4, WWWW));
    ins(s, 2, 3, D(TR, 8, 1), S(TR, 8, XXXX), S(TR, 8, YYYY));
    ins(s, 10, 3, D(TR, 8, 1), S(TR, 8, XYZW), S(TV, 4, WWWW));
    ins(s, 2, 3, D(TR, 8, 3), S(TV, 4, WWWW), NS(TR, 8, XYZW));
    ins(s, 5, 3, D(TR, 8, 3), S(TR, 8, XYZW), S(TR, 8, XYZW));
    ins(s, 2, 3, D(TR, 8, 2), S(TR, 8, YYYY), NS(TR, 8, XXXX));
    ins(s, 4, 4, D(TR, 8, 8), S(TC, 9, YYYY), S(TR, 8, YYYY), S(TR, 8, XXXX));
    ins(s, 5, 3, D(TR, 8, 8), S(TR, 8, WWWW), S(TC, 9, ZZZZ));
    ins(s, 5, 3, D(TR, 8, 4), S(TR, 8, ZZZZ), S(TC, 9, XXXX));
    ins(s, 2, 3, D(TR, 8, 15), NS(TR, 8, XYZW), S(TV, 4, WWWW));
    ins(s, 5, 3, D(TR, 8, 8), S(TR, 8, ZZZZ), S(TR, 8, WWWW));
    ins(s, 8, 3, D(TR, 10, 1), S(TV, 1, XYZW), NS(TC, 5, XYZW));
    ins(s, 11, 3, D(TR, 10, 1), S(TR, 10, XXXX), NS(TR, 10, XXXX));
    ins(s, 2, 3, D(TR, 10, 2), S(TV, 4, WWWW), NS(TR, 10, XXXX));
    ins(s, 5, 3, D(TR, 10, 3), S(TR, 10, XYZW), S(TR, 8, WWWW));
    ins(s, 5, 3, D(TATTR, 0, 8), S(TR, 8, WWWW), S(TC, 12, ZZZZ));
    ins(s, 5, 3, D(TATTR, 1, 15), S(TR, 10, XXXY), S(TC, 12, ZZZZ));
}
static void end(RFShader *s) { emit(s, 0x0000FFFFu); }

static void build_environment(RFShader *s) {       /* vs37: position v0 */
    begin(s, 0); project(s, TV, 0);
    ins(s, 9, 3, D(TTEX, 0, 1), S(TV, 4, XYZW), S(TC, 13, XYZW));
    ins(s, 9, 3, D(TTEX, 0, 2), S(TV, 4, XYZW), S(TC, 14, XYZW));
    fog_chain(s, TV, 0); end(s);
}
static void build_model(RFShader *s) {             /* vs05: position r0 through the node matrix */
    begin(s, 0); node(s); project(s, TR, 0);
    ins(s, 1, 2, D(TTEX, 0, 3), S(TV, 4, XYZW));
    fog_chain(s, TR, 0); end(s);
}
static void build_plane_form(RFShader *s) {        /* vs25/vs28: dp3 then add .w, and oFog */
    begin(s, 0); node(s); project(s, TR, 0);
    ins(s, 8, 3, D(TR, 8, 4), S(TR, 0, XYZW), S(TC, 6, XYZW));
    ins(s, 2, 3, D(TR, 8, 4), S(TR, 8, ZZZZ), S(TC, 6, WWWW));
    ins(s, 2, 3, D(TRAST, 1, 1), S(TV, 4, WWWW), NS(TR, 8, ZZZZ));
    ins(s, 8, 3, D(TR, 8, 2), S(TR, 0, XYZW), S(TC, 8, XYZW));
    ins(s, 2, 3, D(TR, 8, 2), S(TC, 8, WWWW), S(TR, 8, YYYY));  /* the .w first, as the rewrite allows */
    ins(s, 8, 3, D(TR, 8, 1), S(TR, 0, XYZW), S(TC, 7, XYZW));
    ins(s, 2, 3, D(TR, 8, 1), S(TR, 8, XXXX), S(TC, 7, WWWW));
    ins(s, 1, 2, D(TTEX, 1, 3), S(TR, 8, XYZW));
    end(s);
}
static void build_fog_plane_pass(RFShader *s) {    /* vs11: fog as texture coordinates */
    begin(s, 0); project(s, TV, 0);
    ins(s, 9, 3, D(TTEX, 0, 1), S(TV, 0, XYZW), S(TC, 6, XYZW));
    ins(s, 1, 2, D(TTEX, 0, 2), S(TV, 4, ZZZZ));
    ins(s, 9, 3, D(TTEX, 1, 2), S(TV, 0, XYZW), S(TC, 7, XYZW));
    ins(s, 9, 3, D(TTEX, 1, 1), S(TV, 0, XYZW), S(TC, 8, XYZW));
    end(s);
}
static void build_skinned(RFShader *s) {           /* vs32/vs38: two-bone skinning through a0, then oFog */
    begin(s, 1);
    ins(s, 5, 3, D(TR, 0, 3), S(TV, 5, XYZW), S(TC, 9, WWWW));
    ins(s, 2, 3, D(TR, 0, 3), S(TR, 0, XYZW), S(TC, 5, WWWW));
    ins(s, 1, 2, D(TA, 0, 1), S(TR, 0, XXXX));
    for (unsigned k = 0; k < 3; ++k) ins(s, 5, 3, D(TR, 4 + k, 15), S(TV, 6, XXXX), REL(29 + k));
    ins(s, 1, 2, D(TA, 0, 1), S(TR, 0, YYYY));
    for (unsigned k = 0; k < 3; ++k) ins(s, 4, 4, D(TR, 4 + k, 15), S(TV, 6, YYYY), REL(29 + k), S(TR, 4 + k, XYZW));
    for (unsigned k = 0; k < 3; ++k) ins(s, 9, 3, D(TR, 0, 1u << k), S(TV, 0, XYZW), S(TR, 4 + k, XYZW));
    ins(s, 1, 2, D(TR, 0, 8), S(TV, 4, WWWW));
    project(s, TR, 0);
    ins(s, 9, 3, D(TR, 8, 4), S(TR, 0, XYZW), S(TC, 6, XYZW));
    ins(s, 5, 3, D(TR, 8, 4), S(TR, 8, ZZZZ), S(TC, 9, XXXX));
    ins(s, 2, 3, D(TRAST, 1, 1), S(TV, 4, WWWW), NS(TR, 8, ZZZZ));
    ins(s, 9, 3, D(TTEX, 0, 1), S(TR, 0, XYZW), S(TC, 8, XYZW));
    end(s);
}

/* ---------- MojoShader ---------- */

static int rf_binds(const ms_shader *s, int reg) {
    for (int i = 0; i < s->uniform_count; ++i)
        if (s->uniforms[i].type == MS_UNIFORM_FLOAT4 && s->uniforms[i].index <= reg && reg < s->uniforms[i].index + s->uniforms[i].count) return 1;
    return 0;
}
static int translated;
/* Opt-in, transient MSL export for an actual Metal compiler check. The caller
 * creates the destination directory; decoded game shaders never enter the
 * repository. Both variants are exported so compiler errors can be compared. */
static void rf_export_msl(const char *name, const char *variant, const char *source) {
    const char *dir = getenv("HALO_RADIAL_FOG_MSL_DIR");
    if (!dir || !*dir) return;
    char basename[128]; size_t j = 0;
    for (; name[j] && j + 1 < sizeof basename; ++j) {
        unsigned char c = (unsigned char)name[j];
        basename[j] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? (char)c : '_';
    }
    basename[j] = 0;
    size_t capacity = strlen(dir) + strlen(basename) + strlen(variant) + 16;
    char *path = malloc(capacity);
    CHECK(path != NULL, "MSL export path allocation");
    if (!path) return;
    snprintf(path, capacity, "%s/%s-%s.metal", dir, basename, variant);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL, "cannot open MSL export %s", path);
    if (file) {
        size_t length = strlen(source);
        CHECK(fwrite(source, 1, length, file) == length, "cannot write MSL export %s", path);
        CHECK(fclose(file) == 0, "cannot close MSL export %s", path);
    }
    free(path);
}
static void rf_translate(const char *name, const uint32_t *tokens, size_t bytes, int mode) {
    uint32_t *rw = NULL; size_t rwb = 0;
    if (halo_radial_fog_rewrite(tokens, bytes, mode, &rw, &rwb) <= 0) return;
    ms_shader a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    int ea = ms_translate_shader(tokens, bytes, MS_STAGE_VERTEX, "rf_original", &a);
    int eb = ms_translate_shader(rw, rwb, MS_STAGE_VERTEX, "rf_radial", &b);
    CHECK(!ea, "%s: original does not translate: %s", name, a.error);
    CHECK(!eb, "%s: rewritten does not translate: %s", name, b.error);
    if (!ea && !eb) {
        CHECK(rf_binds(&b, HALO_RADIAL_FOG_EYE), "%s: rewritten shader does not bind c4", name);
        CHECK(a.output_count == b.output_count && a.attribute_count == b.attribute_count, "%s: interface changed", name);
        for (int i = 0; i < a.output_count && i < b.output_count; ++i)
            CHECK(a.outputs[i].usage == b.outputs[i].usage && a.outputs[i].index == b.outputs[i].index, "%s: output %d changed", name, i);
        CHECK(strstr(b.source, "FLT_MAX : rsqrt") != NULL, "%s: rsq(0) is not guarded", name);
        rf_export_msl(name, "original", a.source);
        rf_export_msl(name, "radial", b.source);
        translated++;
    }
    ms_shader_destroy(&a); ms_shader_destroy(&b); free(rw);
}

/* ---------- Halo's own shaders ---------- */

static uint8_t *rf_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return b;
}
/* shaders\vsh.bin: TEA with Halo's key over 8-byte blocks (the last,
 * overlapping block first when the size is not a multiple of 8), then a
 * 33-byte trailer, then records of (u32 size, shader tokens). */
static void rf_tea_block(uint8_t *p) {
    static const uint32_t k[4] = { 0x3FFFFFDDu, 0x7FC3u, 0xE5u, 0x3FFFEFu };
    uint32_t v0, v1, sum = 0xC6EF3720u; memcpy(&v0, p, 4); memcpy(&v1, p + 4, 4);
    for (int i = 0; i < 32; ++i) {
        v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
        v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
        sum -= 0x9E3779B9u;
    }
    memcpy(p, &v0, 4); memcpy(p + 4, &v1, 4);
}
typedef struct { uint32_t *tokens; size_t bytes; } RFHaloShader;
static int rf_halo_shaders(RFHaloShader out[64]) {
    size_t n = 0; uint8_t *d = rf_read(rf_fixture_path("HALO_RADIAL_FOG_VSH", "../Halo-PC/baseline/Shaders/vsh.bin"), &n);
    if (!d || n < 64) {
        CHECK(getenv("HALO_RADIAL_FOG_VSH") == NULL, "explicit HALO_RADIAL_FOG_VSH fixture is missing or truncated");
        free(d); return 0;
    }
    if (n % 8) rf_tea_block(d + n - 8);
    for (size_t i = 0; i + 8 <= n; i += 8) rf_tea_block(d + i);
    size_t body = n - 0x21, at = 0; int count = 0;
    while (at + 4 <= body && count < 64) {
        uint32_t size; memcpy(&size, d + at, 4); at += 4;
        if (size < 8 || size > body - at || size % 4) break;
        out[count].tokens = malloc(size);
        if (!out[count].tokens) { CHECK(0, "shader fixture allocation"); break; }
        memcpy(out[count].tokens, d + at, size); out[count].bytes = size;
        if (out[count].tokens[0] != 0xFFFE0101u || out[count].tokens[size / 4 - 1] != 0x0000FFFFu) { free(out[count].tokens); break; }
        count++; at += size;
    }
    free(d);
    if (at != body) {
        for (int i = 0; i < count; ++i) free(out[i].tokens);
        return -1;
    }
    return count;
}
/* Every instruction that reads c6, c7 or c8 must be a term the rewrite
 * handles, or the add of .w that completes a dp3 term. */
static int rf_uncovered_reads(const uint32_t *tokens, size_t bytes) {
    size_t count = bytes / 4, previous = 0; int uncovered = 0; const uint32_t *end = tokens + count;
    for (size_t i = 1; i < count;) {
        uint32_t tok = tokens[i]; unsigned op = tok & 0xFFFFu;
        if (op == 0xFFFFu) break;
        if (op == 0xFFFEu) { i += 1 + ((tok >> 16) & 0x7FFFu); continue; }
        int n = halo_rf_operands(op); if (n < 0) return -1;
        int reads = 0;
        if (op != 31 && op != 81) for (int k = 2; k <= n; ++k)
            if (halo_rf_type(tokens[i + k]) == 2 && halo_rf_number(tokens[i + k]) >= 6 && halo_rf_number(tokens[i + k]) <= 8) reads = 1;
        if (reads && !halo_rf_term(tokens + i, n, end, HALO_RADIAL_FOG_VIEW_PLANE)) {
            int completes = previous && (tokens[previous] & 0xFFFFu) == 8 && previous + 4 == i &&
                            halo_rf_term(tokens + previous, 3, end, HALO_RADIAL_FOG_VIEW_PLANE);   /* the add after a dp3 term */
            if (!completes) uncovered++;
        }
        previous = i;
        i += 1 + (size_t)n;
    }
    return uncovered;
}

/* ---------- the translated engine: 005176D0 and 00518F40 ---------- */

#if RF_HAVE_ENGINE
enum { RF_DEVICE = 0x00B00000u, RF_VTABLE = 0x00B01000u, RF_FOG = 0x00B02000u, RF_STACK = 0x00C00000u,
       RF_RETURN = 0x00DEAD00u, RF_SET_VS = 0xFE100178u, RF_SET_RS = 0xFE1000E4u, RF_SET_TRANSFORM = 0xFE1000B0u };
static float rf_uploaded[256][4]; static uint8_t rf_uploaded_set[256]; static unsigned rf_uploads;
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, engine_flat_base + a, 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(engine_flat_base + a, &v, 4); }
static void wrf(uint32_t a, float v) { memcpy(engine_flat_base + a, &v, 4); }
static void wr16(uint32_t a, uint16_t v) { memcpy(engine_flat_base + a, &v, 2); }
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    uint32_t sp = cpu->gpr[4], ret = rd32(sp); unsigned args;
    if (address == RF_SET_VS) {
        uint32_t start = rd32(sp + 8), data = rd32(sp + 12), count = rd32(sp + 16);
        for (uint32_t r = 0; r < count && start + r < 256; ++r) {
            memcpy(rf_uploaded[start + r], engine_flat_base + data + 16 * r, 16); rf_uploaded_set[start + r] = 1;
        }
        rf_uploads++; args = 4;
    } else if (address == RF_SET_RS || address == RF_SET_TRANSFORM) args = 3;
    else { printf("FAIL unexpected guest call to %08X from %08X\n", address, ret); failures++; args = 0; }
    cpu->gpr[4] = sp + 4 + 4 * args; cpu->gpr[0] = 0; cpu->pc = ret;
}
static int rf_engine_ready;
static int rf_engine_init(void) {
    size_t n = 0;
    uint8_t *rdata = rf_read(rf_fixture_path("HALO_RADIAL_FOG_RDATA", "assets/engine-static/0063a000.bin"), &n);
    if (!rdata) { CHECK(getenv("HALO_RADIAL_FOG_RDATA") == NULL, "explicit HALO_RADIAL_FOG_RDATA fixture is missing"); return 0; }
    engine_flat_base = mmap(NULL, UINT64_C(1) << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (engine_flat_base == MAP_FAILED) { engine_flat_base = NULL; free(rdata); return 0; }
    memcpy(engine_flat_base + 0x0063A000u, rdata, n); free(rdata);
    wr32(RF_DEVICE, RF_VTABLE); wr32(RF_VTABLE + 0x178, RF_SET_VS); wr32(RF_VTABLE + 0xE4, RF_SET_RS); wr32(RF_VTABLE + 0xB0, RF_SET_TRANSFORM);
    wr32(0x0071D174u, RF_DEVICE);
    wr32(0x007C118Cu, 0xFFFF0200u);      /* ps_2_0 hardware: 00518F40 skips the fixed-function transforms */
    engine_flat_base[0x006893FCu] = 1;   /* D3D fog enabled */
    engine_flat_base[0x00689407u] = 1;   /* atmospheric fog: without it 00517720 turns the fog off */
    engine_flat_base[0x00689408u] = 1;   /* planar fog */
    wrf(0x007C1268u, 0.0625f);           /* k */
    wr32(0x00686B04u, 0x00B03000u); wrf(0x00B03000u, 0.1f); wrf(0x00B03004u, 0.2f); wrf(0x00B03008u, 0.3f);
    return rf_engine_ready = 1;
}
static void rf_call(EngineCPU *cpu, uint32_t entry, void (*fn)(EngineCPU *), const uint32_t *args, int argc) {
    cpu->gpr[4] = RF_STACK;
    for (int i = argc - 1; i >= 0; --i) { cpu->gpr[4] -= 4; wr32(cpu->gpr[4], args[i]); }
    cpu->gpr[4] -= 4; wr32(cpu->gpr[4], RF_RETURN);
    cpu->pc = entry; fn(cpu);
    CHECK(cpu->pc == RF_RETURN, "%08X did not return (pc %08X)", entry, cpu->pc);
}
/* The view constants the translated engine uploads for one view. */
static void rf_engine_constants(RFEnv *e, const RFView *v, const RFFog *f) {
    float c0_3[4][4]; memcpy(c0_3, e->c, sizeof c0_3);
    EngineCPU cpu; memset(&cpu, 0, sizeof cpu); engine_fp_init(&cpu); engine_fp_control(&cpu, 0x027F);
    for (int i = 0; i < 3; ++i) { wrf(0x007C1228u + 4 * i, (float)v->eye[i]); wrf(0x007C1234u + 4 * i, (float)v->fwd[i]); }
    /* The view's fog record, copied to 007C1408 by 005176D0. */
    memset(engine_flat_base + RF_FOG, 0, 0x50);
    wrf(RF_FOG + 0x04, 0.62f); wrf(RF_FOG + 0x08, 0.635f); wrf(RF_FOG + 0x0C, 0.80f);
    wrf(RF_FOG + 0x10, (float)f->density); wrf(RF_FOG + 0x14, (float)f->near_); wrf(RF_FOG + 0x18, (float)f->far_);
    wr16(RF_FOG + 0x1C, (uint16_t)f->type);
    for (int i = 0; i < 4; ++i) wrf(RF_FOG + 0x20 + 4 * i, (float)f->plane[i]);
    wrf(RF_FOG + 0x3C, (float)f->planar_density); wrf(RF_FOG + 0x40, (float)f->a); wrf(RF_FOG + 0x44, (float)f->b);
    memset(rf_uploaded_set, 0, sizeof rf_uploaded_set);
    cpu.gpr[2] = RF_FOG; rf_call(&cpu, 0x005176D0u, sub_005176D0, NULL, 0);
    const uint32_t minus_one[2] = { 0xBF800000u, 0xBF800000u };
    rf_call(&cpu, 0x00518F40u, sub_00518F40, minus_one, 2);
    for (int r = 4; r <= 9; ++r) { CHECK(rf_uploaded_set[r], "engine did not upload c%d", r); memcpy(e->c[r], rf_uploaded[r], 16); }
    memcpy(e->c, c0_3, sizeof c0_3);   /* the test's own projection: the globals behind c0..c3 are not set up */
    memset(e->fog_taint, 0, sizeof e->fog_taint);
    e->fog_taint[6] = e->fog_taint[8] = 1; e->fog_taint[7] = f->type == 2;
    CHECK(rd32(0x007C1424u) == (uint32_t)f->type, "planar fog type %u, wanted %d", rd32(0x007C1424u), f->type);
}
static void rf_engine_model(RFEnv *e, const RFView *v, const RFFog *f) { rf_constants(e, v, f); rf_engine_constants(e, v, f); }
#endif

static const RFFog rf_b30 = { 5, 90, 0.5, 0, { 0, 0, 1, -2 }, 0, 1, 1, 0.0625 };          /* sky "clear afternoon" */
static const RFFog rf_water = { 5, 90, 0.5, 1, { 0, 0, 1, -2 }, 1, 0.1, 0.15, 0.0625 };   /* levels\b30\water */
static const RFFog rf_planeless = { 5, 90, 0.5, 2, { 0, 0, 0, 0 }, 0.8, 6, 0.15, 0.0625 };

static void rf_fixed_checks(void) {
    /* A 3-4-12 triangle has length exactly 13. Choose each fog curve's
     * parameters independently so the expected transmittance is exactly 1/2. */
    const double point[3] = { 15, -36, 15 }, eye[3] = { 12, -40, 3 };
    const float densities[] = { 0, 0.053319014f, 0.064042663f, 0 };
    float first[4] = {0}; double worst = 0;
    for (int b = 0; b < BEARINGS; ++b) {
        double fwd[3], right[3], up[3], delta[3];
        rf_bearing(b, fwd); rf_view_axes(fwd, right, up);
        for (int i = 0; i < 3; ++i) delta[i] = point[i] - eye[i];
        const float view[3] = { (float)dot3(delta, right), (float)dot3(delta, up), (float)dot3(delta, fwd) };
        for (unsigned mode = 0; mode <= 3; ++mode) {
            float got = halo_fixed_radial_fog(view, mode, 3, 23, densities[mode], 0.375f);
            float expected = mode == 0 ? 0.375f : 0.5f;
            CHECK(fabsf(got - expected) < 3e-7f, "fixed fog mode %u bearing %d: %.9g expected %.9g", mode, b, got, expected);
            if (!b) first[mode] = got;
            double spread = fabs((double)got - first[mode]); if (spread > worst) worst = spread;
            CHECK(spread < 3e-7, "fixed fog mode %u changes between bearings", mode);
        }
    }
    const float zero[3] = {0}, at_start[3] = {3, 0, 0}, at_end[3] = {0, 23, 0}, far[3] = {0, 0, 100};
    CHECK(halo_fixed_radial_fog(zero, 3, 3, 23, 0, 0) == 1, "fixed linear near clamp");
    CHECK(halo_fixed_radial_fog(at_start, 3, 3, 23, 0, 0) == 1, "fixed linear start");
    CHECK(halo_fixed_radial_fog(at_end, 3, 3, 23, 0, 0) == 0, "fixed linear end");
    CHECK(halo_fixed_radial_fog(far, 3, 3, 23, 0, 0) == 0, "fixed linear far clamp");
    CHECK(halo_fixed_radial_fog(zero, 3, 3, 3, 0, 0) == 1, "fixed degenerate range before edge");
    CHECK(halo_fixed_radial_fog(at_start, 3, 3, 3, 0, 0) == 0, "fixed degenerate range at edge");
    for (unsigned mode = 1; mode <= 2; ++mode) {
        CHECK(halo_fixed_radial_fog(zero, mode, 0, 0, 1, 0) == 1, "fixed exponential at eye mode %u", mode);
        CHECK(halo_fixed_radial_fog(far, mode, 0, 0, 0, 0) == 1, "fixed zero density mode %u", mode);
        CHECK(halo_fixed_radial_fog(far, mode, 0, 0, 1, 0) < 1e-30f, "fixed opaque exponential mode %u", mode);
    }
    CHECK(halo_fixed_radial_fog(far, 0, 0, 0, 0, -2) == 0, "fixed supplied negative clamp");
    CHECK(halo_fixed_radial_fog(far, 0, 0, 0, 0, 2) == 1, "fixed supplied positive clamp");
    printf("fixed function: linear, exp, exp2 and supplied fog across %d bearings; maximum absolute spread %.3g\n", BEARINGS, worst);
}

static void rf_plane_checks(void) {
    /* Direct, independent expectations for the fog-plane pass. An actual
     * world plane must stay a plane; only c8's viewing depth becomes range.
     * Type 2 has no world plane: its c7 is the reversed view-depth plane and
     * must become k-range as well. Test the eye and both sides of a plane. */
    RFShader shader; build_fog_plane_pass(&shader);
    const double eye[3] = {12, -40, 3};
    const double deltas[3][3] = {{0, 0, 0}, {3, 4, 12}, {0, 0, -13}};
    double worst = 0; unsigned checked = 0;
    for (int type = 1; type <= 2; ++type) {
        RFFog fog = {5, 90, .5, type, {0, 0, 1, 6}, .8, 4, 2, .0625};
        uint32_t *rewritten = NULL; size_t rewritten_bytes = 0;
        int terms = halo_radial_fog_rewrite(shader.t, shader.n * 4, rf_mode(&fog), &rewritten, &rewritten_bytes);
        CHECK(terms == (type == 1 ? 2 : 3), "type %d raw plane term count %d", type, terms);
        if (!rewritten) continue;
        for (unsigned p = 0; p < 3; ++p) for (int b = 0; b < BEARINGS; ++b) {
            RFView view; memcpy(view.eye, eye, sizeof eye); rf_bearing(b, view.fwd);
            double point[3]; for (int c = 0; c < 3; ++c) point[c] = eye[c] + deltas[p][c];
            double distance = sqrt(dot3(deltas[p], deltas[p]));
            RFEnv env = {0}; rf_inputs(&env, point); rf_constants(&env, &view, &fog);
            RFOut original, radial;
            CHECK(rf_run(shader.t, shader.n * 4, &env, &original) &&
                  rf_run(rewritten, rewritten_bytes, &env, &radial), "raw plane shader run");
            const double expected[] = {(distance - 5) / 85, distance / 4,
                                       type == 1 ? (6 - point[2]) / 2 : .0625 - distance};
            const float actual[] = {radial.o[O_T0][0], radial.o[O_T0 + 1][0], radial.o[O_T0 + 1][1]};
            for (unsigned term = 0; term < 3; ++term) {
                double error = fabs(actual[term] - expected[term]);
                CHECK(error < 5e-6, "type %d point %u bearing %d term %u: %.9g expected %.9g",
                      type, p, b, term, actual[term], expected[term]);
                if (error > worst) worst = error;
            }
            if (type == 1)
                CHECK(rf_output_equal(&original, &radial, O_T0 + 1, 1), "world fog plane changed in bearing %d", b);
            checked++;
        }
        free(rewritten);
    }
    CHECK(checked == 54, "raw plane coverage %u", checked);
    printf("fog planes: %u world-point/bearing cases, type-1 c7 bit-identical, type-2 c7=k-range, c8=range/a; max absolute error %.3g\n", checked, worst);
}

static void rf_precision_checks(void) {
    /* Keep the eye and eye-relative vertex displacement exactly representable
     * in float. This isolates cancellation in Halo's uploaded plane constants
     * from vertex-quantization error. The last origin is deliberately far
     * beyond a normal level; report the remaining finite-precision limitation. */
    RFShader s; begin(&s, 0); project(&s, TV, 0);
    ins(&s, 9, 3, D(TTEX, 0, 1), S(TV, 0, XYZW), S(TC, 6, XYZW)); end(&s);
    uint32_t *rw = NULL; size_t bytes = 0;
    CHECK(halo_radial_fog_rewrite(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH, &rw, &bytes) == 1, "precision shader rewrite");
    if (!rw) return;
    const double origins[] = { 0, 128, 1024, 8192, 65536, 1048576 };
    printf("atmospheric precision (13 units from eye; raw fog before density):\n");
    for (size_t k = 0; k < sizeof origins / sizeof *origins; ++k) {
        double o = origins[k], worst = 0; float lo = FLT_MAX, hi = -FLT_MAX;
        for (int b = 0; b < BEARINGS; ++b) {
            RFView view = { { o, -o, o / 2 }, {0} }; rf_bearing(b, view.fwd);
            RFEnv e = {0}; double point[3] = { o + 3, -o + 4, o / 2 + 12 };
            rf_inputs(&e, point); rf_constants(&e, &view, &rf_b30);
            RFOut result; CHECK(rf_run(rw, bytes, &e, &result), "precision shader run");
            float got = result.o[O_T0][0]; double error = fabs(got - 8.0 / 85.0);
            CHECK(isfinite(got), "non-finite large-world fog");
            /* Forward-vector/plane packing and dot products incur errors
             * proportional to the world origin, even after radialization. */
            CHECK(error <= 16 * FLT_EPSILON * (1 + o / 85), "origin %.0f bearing %d: fog %.9g error %.3g", o, b, got, error);
            if (error > worst) worst = error;
            lo = fminf(lo, got); hi = fmaxf(hi, got);
        }
        printf("  origin %7.0f: max absolute error %.3g, bearing spread %.3g\n", o, worst, hi - lo);
    }
    free(rw);
}

static void rf_report(const char *name, const RFStats *st) {
    printf("  %-28s %5u vertices x %d bearings: %4u fogged outputs (%u bearing-free), worst vs PC facing it %.2g, "
           "spread across bearings %.2g (original %.3g absolute); differences relative to 1 + |value|\n", name, st->vertices, BEARINGS, st->rewritten_outputs, st->independent,
           st->worst_reference, st->worst_spread, st->original_spread);
}

int main(void) {
    rf_fixed_checks();
    rf_plane_checks();
    rf_precision_checks();
    /* The example the item asks for: one world point, every bearing. A point
     * 60 units out at 30 degrees of bearing (the join between the centre and
     * the +60 bearing) and 50 degrees up (where the ring hands over to the
     * zenith cap), through vs37's fog chain with b30's sky fog. */
    {
        RFShader s; build_environment(&s);
        uint32_t *rw; size_t rwb; int terms = halo_radial_fog_rewrite(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH, &rw, &rwb);
        CHECK(terms == 2, "environment: %d terms", terms);
        double eye[3] = { 12, -40, 3 }, el = 50 * M_PI / 180, az = 30 * M_PI / 180, p[3];
        double dir[3] = { cos(el) * cos(az), cos(el) * sin(az), sin(el) };
        for (int i = 0; i < 3; ++i) p[i] = eye[i] + 60 * dir[i];
        RFEnv e; memset(&e, 0, sizeof e); rf_fill_constants(&e, 1); rf_inputs(&e, p); e.c[12][2] = 1;
        printf("one point 60 units out at bearing 30, elevation 50 (b30 sky fog: density 0.5, start 5, opaque 90):\n");
        printf("  %-10s %-10s %-12s %-12s\n", "bearing", "depth", "PC fog", "radial fog");
        const char *names[] = { "0", "-60", "+60", "+120", "180", "-120", "up", "down", "head" };
        float radial0 = 0;
        for (int b = 0; b < BEARINGS; ++b) {
            if (b == 3 || b == 4 || b == 5 || b == 7) continue;   /* behind or below the point */
            RFView v; memcpy(v.eye, eye, sizeof eye); rf_bearing(b, v.fwd);
            RFEnv be = e; rf_constants(&be, &v, &rf_b30);
            RFOut o, r; CHECK(rf_run(s.t, s.n * 4, &be, &o) && rf_run(rw, rwb, &be, &r), "example run");
            double depth = 60 * dot3(dir, v.fwd);
            /* oD0.w is the fog transmittance times c12.z; report fog = 1 - transmittance. */
            float pc = 1 - o.o[O_D0][3] / be.c[12][2], rad = 1 - r.o[O_D0][3] / be.c[12][2];
            printf("  %-10s %-10.2f %-12.4f %-12.4f\n", names[b], depth, pc, rad);
            if (b == 0) radial0 = rad;
            else CHECK(fabsf(rad - radial0) < 1e-5f, "radial fog differs between bearings: %.6f vs %.6f", rad, radial0);
        }
        CHECK(fabsf(radial0 - 0.5f * (60 - 5) / 85) < 1e-4f, "radial fog %.6f, expected %.6f", radial0, 0.5 * 55 / 85);
        free(rw);
        printf("centre view against PC (b30 sky fog, a point 60 units out; PC is planar depth along the centre axis):\n");
        const double angles[] = { 0, 10, 20, 30, 32, 45, 57 };
        for (size_t i = 0; i < sizeof angles / sizeof *angles; ++i) {
            double th = angles[i] * M_PI / 180, z = 60 * cos(th);
            double pc = 0.5 * clamp01((z - 5) / 85), rad = 0.5 * clamp01((60 - 5) / 85.0);
            printf("  %4.0f deg off axis: depth x%.4f, fog %.4f -> %.4f (%+.4f)\n", angles[i], 1 / cos(th), pc, rad, rad - pc);
        }
    }

    /* Part 1: synthetic shaders. */
    struct { const char *name; void (*build)(RFShader *); int terms_depth, terms_plane; } shapes[] = {
        { "environment (vs37)", build_environment, 2, 3 }, { "model (vs05)", build_model, 2, 3 },
        { "dp3 + .w, oFog (vs25/vs28)", build_plane_form, 2, 3 }, { "fog-plane pass (vs11)", build_fog_plane_pass, 2, 3 },
        { "skinned, a0 (vs32/vs38)", build_skinned, 2, 2 } };
    const RFFog *fogs[] = { &rf_b30, &rf_water, &rf_planeless };
    const char *fog_names[] = { "sky fog only", "fog plane (type 1)", "planeless fog (type 2)" };
    printf("synthetic shaders:\n");
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; ++i) {
        RFShader s; shapes[i].build(&s);
        CHECK(halo_radial_fog_terms(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH) == shapes[i].terms_depth, "%s: depth terms %d", shapes[i].name,
              halo_radial_fog_terms(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH));
        CHECK(halo_radial_fog_terms(s.t, s.n * 4, HALO_RADIAL_FOG_VIEW_PLANE) == shapes[i].terms_plane, "%s: view-plane terms", shapes[i].name);
        CHECK(rf_uncovered_reads(s.t, s.n * 4) == 0, "%s: fog constant read the rewrite does not cover", shapes[i].name);
        for (size_t f = 0; f < 3; ++f) {
            RFStats st = {0}; char label[96]; snprintf(label, sizeof label, "%s, %s", shapes[i].name, fog_names[f]);
            rf_differential(label, s.t, s.n * 4, fogs[f], 1500, rf_constants, &st);
            CHECK(st.vertices > 1000 && st.independent > 0, "%s: %u vertices, %u bearing-free outputs", label, st.vertices, st.independent);
            if (f == 0) rf_report(shapes[i].name, &st);
        }
        rf_translate(shapes[i].name, s.t, s.n * 4, HALO_RADIAL_FOG_VIEW_PLANE);
    }
    /* Saturation belongs to the final radial result, after the correction.
     * Include negative, interior and >1 values, including multi-lane writes. */
    {
        RFShader s; begin(&s, 0); project(&s, TV, 0);
        ins(&s, 9, 3, D(TTEX, 0, 5) | 0x00100000u, S(TV, 0, XYZW), S(TC, 6, XYZW));
        end(&s);
        RFStats stats = {0}; rf_differential("saturated dp4", s.t, s.n * 4, &rf_b30, 1500, rf_constants, &stats);
        CHECK(stats.vertices == 1500 && stats.independent == 3000, "saturated dp4 coverage %u/%u", stats.vertices, stats.independent);
        rf_translate("saturated dp4", s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH);
    }
    /* A shader with every temporary in use is left alone, as is anything the
     * rewrite does not understand. */
    {
        RFShader s; build_environment(&s); s.n--;
        for (unsigned r = 0; r < 12; ++r) ins(&s, 1, 2, D(TR, r, 15), S(TV, 0, XYZW));
        end(&s);
        uint32_t *rw = (uint32_t *)1; size_t rwb = 7;
        CHECK(halo_radial_fog_rewrite(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH, &rw, &rwb) == 0 && !rw && !rwb, "no free temporary");
        RFShader v; build_environment(&v); v.t[0] = 0xFFFE0200u;
        CHECK(halo_radial_fog_terms(v.t, v.n * 4, HALO_RADIAL_FOG_DEPTH) == 0, "vs_2_0 accepted");
        build_environment(&v);
        for (int mode = -1; mode <= 3; ++mode) if (mode != HALO_RADIAL_FOG_DEPTH && mode != HALO_RADIAL_FOG_VIEW_PLANE) {
            rw = (uint32_t *)1; rwb = 7;
            CHECK(halo_radial_fog_terms(v.t, v.n * 4, mode) == 0, "disabled/invalid mode %d counted terms", mode);
            CHECK(halo_radial_fog_rewrite(v.t, v.n * 4, mode, &rw, &rwb) == 0 && !rw && !rwb, "disabled/invalid mode %d rewrote shader", mode);
        }
        for (size_t cut = 0; cut < v.n; ++cut)
            CHECK(halo_radial_fog_terms(v.t, cut * 4, HALO_RADIAL_FOG_DEPTH) == 0, "truncated shader accepted at token %zu", cut);
        const uint32_t truncated_comment[] = { 0xFFFE0101u, 0x7FFFFFFEu, 0xFFFFu };
        CHECK(halo_radial_fog_terms(truncated_comment, sizeof truncated_comment, HALO_RADIAL_FOG_DEPTH) == 0, "truncated comment accepted");
        CHECK(halo_radial_fog_terms(v.t, (1u << 20) + 4, HALO_RADIAL_FOG_DEPTH) == 0, "oversize bytecode accepted");
        CHECK(halo_radial_fog_terms(v.t, (v.n - 1) * 4, HALO_RADIAL_FOG_DEPTH) == 0, "missing end token accepted");
        CHECK(halo_radial_fog_terms(v.t, v.n * 4 - 2, HALO_RADIAL_FOG_DEPTH) == 0, "ragged size accepted");
        build_environment(&v); v.n--; ins(&v, 37, 3, D(TR, 1, 15), S(TV, 0, XYZW), S(TV, 0, XYZW)); end(&v);
        CHECK(halo_radial_fog_terms(v.t, v.n * 4, HALO_RADIAL_FOG_DEPTH) == 0, "unknown opcode accepted");
        for (unsigned matrix = 20; matrix <= 24; ++matrix) {
            build_environment(&v); v.n--;
            ins(&v, matrix, 3, D(TR, 1, 15), S(TV, 0, XYZW), S(TC, 10, XYZW)); end(&v);
            rw = (uint32_t *)1; rwb = 7;
            CHECK(halo_radial_fog_rewrite(v.t, v.n * 4, HALO_RADIAL_FOG_DEPTH, &rw, &rwb) == 0 && !rw && !rwb,
                  "matrix opcode %u with implicit register span accepted", matrix);
        }
        RFShader lone; begin(&lone, 0); project(&lone, TV, 0);
        ins(&lone, 8, 3, D(TR, 8, 4), S(TV, 0, XYZW), S(TC, 6, XYZW));      /* dp3 with no .w add: not a plane */
        ins(&lone, 9, 3, D(TR, 8, 2), S(TV, 0, 0x24u), S(TC, 8, XYZW));     /* swizzled position */
        ins(&lone, 9, 3, D(TR, 8, 1), S(TV, 0, XYZW), NS(TC, 6, XYZW));     /* negated constant */
        ins(&lone, 1, 2, D(TATTR, 0, 15), S(TR, 8, XYZW)); end(&lone);
        CHECK(halo_radial_fog_terms(lone.t, lone.n * 4, HALO_RADIAL_FOG_VIEW_PLANE) == 0, "unrecognised forms taken as terms");
        CHECK(rf_uncovered_reads(lone.t, lone.n * 4) == 3, "uncovered reads not detected");
        RFShader saturated_dp3; begin(&saturated_dp3, 0); project(&saturated_dp3, TV, 0);
        uint32_t saturated_dst = D(TR, 8, 4) | 0x00100000u;
        ins(&saturated_dp3, 8, 3, saturated_dst, S(TV, 0, XYZW), S(TC, 6, XYZW));
        ins(&saturated_dp3, 2, 3, saturated_dst, S(TR, 8, ZZZZ), S(TC, 6, WWWW));
        end(&saturated_dp3);
        CHECK(halo_radial_fog_terms(saturated_dp3.t, saturated_dp3.n * 4, HALO_RADIAL_FOG_DEPTH) == 0,
              "saturated intermediate dp3 accepted despite non-affine cancellation");
        const uint32_t bad_positions[] = { S(TV, 16, XYZW), S(TR, 12, XYZW), S(TV, 0, XYZW) & 0x7FFFFFFFu };
        for (size_t i = 0; i < sizeof bad_positions / sizeof *bad_positions; ++i) {
            RFShader invalid; begin(&invalid, 0); project(&invalid, TV, 0);
            ins(&invalid, 9, 3, D(TTEX, 0, 1), bad_positions[i], S(TC, 6, XYZW)); end(&invalid);
            CHECK(halo_radial_fog_terms(invalid.t, invalid.n * 4, HALO_RADIAL_FOG_DEPTH) == 0, "invalid position %08x accepted", bad_positions[i]);
        }
        CHECK(halo_radial_fog_rewrite(NULL, 0, HALO_RADIAL_FOG_DEPTH, &rw, &rwb) == 0 && !rw, "null tokens");
        CHECK(halo_radial_fog_rewrite(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH, NULL, &rwb) == 0, "null output");
        rw = (uint32_t *)1;
        CHECK(halo_radial_fog_rewrite(s.t, s.n * 4, HALO_RADIAL_FOG_DEPTH, &rw, NULL) == 0 && !rw, "null output size");
    }

    /* Part 2: the constants from the translated engine. */
#if RF_HAVE_ENGINE
    if (rf_engine_init()) {
        printf("translated 005176D0/00518F40 constants:\n");
        double worst = 0; int views = 0;
        for (size_t f = 0; f < 3; ++f) for (int b = 0; b < BEARINGS; ++b) {
            RFView v = { { 31.5, -12.25, 4.5 }, { 0, 0, 0 } }; rf_bearing(b, v.fwd);
            RFEnv model, engine; memset(&model, 0, sizeof model); rf_constants(&model, &v, fogs[f]);
            engine = model; rf_engine_constants(&engine, &v, fogs[f]);
            for (int c = 0; c < 3; ++c)
                CHECK(engine.c[4][c] == (float)v.eye[c] && engine.c[5][c] == (float)v.fwd[c], "c4/c5 are not (eye, 2)/(fwd, 0.5)");
            CHECK(engine.c[4][3] == 2 && engine.c[5][3] == 0.5f, "c4.w/c5.w");
            for (int r = 6; r <= 9; ++r) for (int c = 0; c < 4; ++c) {
                double d = fabs((double)engine.c[r][c] - model.c[r][c]);
                CHECK(d <= 1e-5 * (1 + fabs(model.c[r][c])), "%s bearing %d: c%d.%d engine %.9g model %.9g", fog_names[f], b, r, c,
                      engine.c[r][c], model.c[r][c]);
                if (d > worst) worst = d;
            }
            views++;
        }
        printf("  %d views: c4 = (eye, 2) and c5 = (fwd, 0.5) exactly; c6..c9 match the model within %.2g\n", views, worst);
        for (size_t i = 0; i < sizeof shapes / sizeof *shapes; ++i) {
            RFShader s; shapes[i].build(&s);
            for (size_t f = 0; f < 3; ++f) {
                RFStats st = {0}; char label[96]; snprintf(label, sizeof label, "%s, %s (engine)", shapes[i].name, fog_names[f]);
                rf_differential(label, s.t, s.n * 4, fogs[f], 150, rf_engine_model, &st);
                CHECK(st.vertices > 100, "%s", label);
                if (f == 2) rf_report(label, &st);
            }
        }
    } else puts("SKIP: translated-engine constants (no assets/engine-static/0063a000.bin)");
#else
    puts("SKIP: translated-engine constants (compile with -DRF_HAVE_ENGINE=1 and the generated directory on the include path)");
#endif

    /* Part 3: Halo's own shaders. */
    RFHaloShader halo[64]; int count = rf_halo_shaders(halo);
    if (count == 64) {
        int fogged = 0, terms = 0, depth_terms = 0; unsigned before = 0, after = 0;
        RFStats all = {0};
        for (int i = 0; i < 64; ++i) {
            char name[32]; snprintf(name, sizeof name, "vsh.bin #%02d", i);
            CHECK(rf_uncovered_reads(halo[i].tokens, halo[i].bytes) == 0, "%s reads a fog constant outside a term", name);
            int t = halo_radial_fog_terms(halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_VIEW_PLANE);
            int t_any = 0;
            for (size_t k = 1; k < halo[i].bytes / 4; ++k)
                if (halo_rf_type(halo[i].tokens[k]) == 2 && halo_rf_number(halo[i].tokens[k]) >= 6 && halo_rf_number(halo[i].tokens[k]) <= 8 &&
                    (halo[i].tokens[k] & 0x80000000u)) t_any = 1;
            CHECK(!t_any || t > 0, "%s reads fog constants but was not rewritten", name);
            if (t <= 0) continue;
            fogged++; terms += t; depth_terms += halo_radial_fog_terms(halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_DEPTH);
            for (size_t f = 0; f < 3; ++f) {
                RFStats st = {0};
                rf_differential(name, halo[i].tokens, halo[i].bytes, fogs[f], 120, rf_constants, &st);
                all.vertices += st.vertices; all.rewritten_outputs += st.rewritten_outputs; all.independent += st.independent;
                if (st.worst_reference > all.worst_reference) all.worst_reference = st.worst_reference;
                if (st.worst_spread > all.worst_spread) all.worst_spread = st.worst_spread;
                if (st.original_spread > all.original_spread) all.original_spread = st.original_spread;
            }
            RFEnv e; memset(&e, 0, sizeof e); double p[3] = { 1, 2, 3 }; rf_inputs(&e, p);
            RFView v = { { 0, 0, 0 }, { 1, 0, 0 } }; rf_constants(&e, &v, &rf_planeless);
            uint32_t *rw; size_t rwb; halo_radial_fog_rewrite(halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_VIEW_PLANE, &rw, &rwb);
            RFOut o, r; rf_run(halo[i].tokens, halo[i].bytes, &e, &o); rf_run(rw, rwb, &e, &r);
            before += o.instructions; after += r.instructions; free(rw);
            rf_translate(name, halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_VIEW_PLANE);
        }
        printf("Halo's vsh.bin: %d of 64 shaders carry %d c6/c8 terms (%d with c7); every read of c6/c7/c8 is covered;\n"
               "  with c7 (the worst case) %u -> %u instructions over those shaders (+%.0f%%)\n",
               fogged, depth_terms, terms, before, after, 100.0 * (after - before) / before);
        rf_report("vsh.bin, all fog types", &all);
        CHECK(fogged == 41 && terms == 94 && depth_terms == 67, "expected 41 fogging shaders with 67 (94) terms, found %d with %d (%d)",
              fogged, depth_terms, terms);
        /* Timing: the rewrite runs once per pipeline compile. */
        double t0 = now(); unsigned runs = 0;
        for (int rep = 0; rep < 200; ++rep) for (int i = 0; i < 64; ++i) {
            uint32_t *rw; size_t rwb;
            if (halo_radial_fog_rewrite(halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_VIEW_PLANE, &rw, &rwb) > 0) { free(rw); runs++; }
        }
        double per = (now() - t0) / runs;
        double t1 = now(); volatile int sink = 0;
        for (int rep = 0; rep < 200; ++rep) for (int i = 0; i < 64; ++i) sink += halo_radial_fog_terms(halo[i].tokens, halo[i].bytes, HALO_RADIAL_FOG_VIEW_PLANE);
        printf("timing: rewrite %.2f us per fog shader (once per pipeline), term scan %.2f us per shader (once per CreateVertexShader)\n",
               per * 1e6, (now() - t1) / (200 * 64) * 1e6);
        for (int i = 0; i < 64; ++i) free(halo[i].tokens);
    } else if (count == 0) printf("SKIP: Halo's shaders (no %s)\n", rf_fixture_path("HALO_RADIAL_FOG_VSH", "../Halo-PC/baseline/Shaders/vsh.bin"));
    else {
        CHECK(0, "vsh.bin did not decode: %d records", count);
        for (int i = 0; i < count; ++i) free(halo[i].tokens);
    }

    printf("MojoShader: %d rewritten shaders translate, bind c4 and guard rsq(0)\n", translated);
    CHECK(translated >= 5, "only %d shaders translated", translated);
    if (failures) { printf("FAIL: %d checks\n", failures); return 1; }
    puts("PASS: radial fog matches bearings and PC facing the point within float tolerance; non-fog outputs remain bit-identical");
    return 0;
}

#if RF_HAVE_ENGINE
#include "engine_flags.h"
#include "engine_registers.h"
#include "sub_005176D0.c"
#include "sub_00518F40.c"
#include "sub_0050C9A0.c"
#include "sub_004AB5D0.c"
#include "sub_0044D9E0.c"
#endif
