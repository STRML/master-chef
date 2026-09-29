#ifndef HALO_ENGINE_REUSE_ENGINE_FLAGS_H
#define HALO_ENGINE_REUSE_ENGINE_FLAGS_H

#include <stdint.h>

/*
 * Shared IA-32 EFLAGS state for statically recompiled engine code.
 *
 * These helpers accept unsigned operands and do all widening before arithmetic,
 * avoiding signed overflow and shift undefined behavior.  width_bits must be
 * 8, 16, or 32; generated code validates the width before emitting a call.  A
 * pointer to the shared CPU flag word makes state ownership explicit.
 *
 * Architecturally undefined outputs are deterministic here: logical operations
 * clear AF; nonzero shifts clear AF, preserve CF when count >= width, and
 * preserve OF when count > 1; two/three-operand IMUL preserves PF/AF/ZF/SF.
 * DIV/IDIV and one-operand IMUL are outside this layer.
 */

#define ENGINE_EFLAGS_CF        UINT32_C(0x00000001)
#define ENGINE_EFLAGS_RESERVED1 UINT32_C(0x00000002)
#define ENGINE_EFLAGS_PF        UINT32_C(0x00000004)
#define ENGINE_EFLAGS_AF        UINT32_C(0x00000010)
#define ENGINE_EFLAGS_ZF        UINT32_C(0x00000040)
#define ENGINE_EFLAGS_SF        UINT32_C(0x00000080)
#define ENGINE_EFLAGS_OF        UINT32_C(0x00000800)

#define ENGINE_EFLAGS_STATUS_MASK \
    (ENGINE_EFLAGS_CF | ENGINE_EFLAGS_PF | ENGINE_EFLAGS_AF | \
     ENGINE_EFLAGS_ZF | ENGINE_EFLAGS_SF | ENGINE_EFLAGS_OF)

static inline uint32_t engine_flags_width_mask(unsigned width_bits) {
    return width_bits == 32u
        ? UINT32_MAX
        : (UINT32_C(1) << width_bits) - UINT32_C(1);
}

static inline uint32_t engine_flags_sign_mask(unsigned width_bits) {
    return UINT32_C(1) << (width_bits - 1u);
}

static inline uint32_t engine_flags_even_parity8(uint32_t value) {
    value ^= value >> 4u;
    value &= UINT32_C(0x0f);
    /* Bit n of 0x9669 is one exactly when n has even parity. */
    return (UINT32_C(0x9669) >> value) & UINT32_C(1);
}

static inline uint32_t engine_flags_common(uint32_t result,
                                            unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t narrowed = result & mask;
    uint32_t bits = 0u;
    if (engine_flags_even_parity8(narrowed)) bits |= ENGINE_EFLAGS_PF;
    if (narrowed == 0u) bits |= ENGINE_EFLAGS_ZF;
    if ((narrowed & sign) != 0u) bits |= ENGINE_EFLAGS_SF;
    return bits;
}

static inline uint32_t engine_flags_add(uint32_t *flags,
                                        uint32_t lhs, uint32_t rhs,
                                        uint32_t carry_in,
                                        unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t a = lhs & mask;
    const uint32_t b = rhs & mask;
    const uint32_t carry = carry_in & UINT32_C(1);
    const uint64_t wide = (uint64_t)a + (uint64_t)b + (uint64_t)carry;
    const uint32_t result = (uint32_t)wide & mask;
    uint32_t bits = engine_flags_common(result, width_bits);
    if (wide > (uint64_t)mask) bits |= ENGINE_EFLAGS_CF;
    if (((a ^ b ^ result) & UINT32_C(0x10)) != 0u)
        bits |= ENGINE_EFLAGS_AF;
    if (((~(a ^ b) & (a ^ result)) & sign) != 0u)
        bits |= ENGINE_EFLAGS_OF;
    *flags = (*flags & ~ENGINE_EFLAGS_STATUS_MASK) | bits;
    return result;
}

static inline uint32_t engine_flags_sub(uint32_t *flags,
                                        uint32_t lhs, uint32_t rhs,
                                        uint32_t borrow_in,
                                        unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t a = lhs & mask;
    const uint32_t b = rhs & mask;
    const uint32_t borrow = borrow_in & UINT32_C(1);
    const uint64_t subtrahend = (uint64_t)b + (uint64_t)borrow;
    const uint32_t result = (a - b - borrow) & mask;
    uint32_t bits = engine_flags_common(result, width_bits);
    if ((uint64_t)a < subtrahend) bits |= ENGINE_EFLAGS_CF;
    if (((a ^ b ^ result) & UINT32_C(0x10)) != 0u)
        bits |= ENGINE_EFLAGS_AF;
    if ((((a ^ b) & (a ^ result)) & sign) != 0u)
        bits |= ENGINE_EFLAGS_OF;
    *flags = (*flags & ~ENGINE_EFLAGS_STATUS_MASK) | bits;
    return result;
}

static inline uint32_t engine_flags_logic(uint32_t *flags, uint32_t result,
                                          unsigned width_bits) {
    const uint32_t narrowed = result & engine_flags_width_mask(width_bits);
    const uint32_t bits = engine_flags_common(narrowed, width_bits);
    /* AF is architecturally undefined for TEST/AND/OR/XOR; normalize it to 0. */
    *flags = (*flags & ~ENGINE_EFLAGS_STATUS_MASK) | bits;
    return narrowed;
}

static inline uint32_t engine_flags_inc(uint32_t *flags, uint32_t value,
                                        unsigned width_bits) {
    const uint32_t saved_cf = *flags & ENGINE_EFLAGS_CF;
    const uint32_t result = engine_flags_add(flags, value, 1u, 0u, width_bits);
    *flags = (*flags & ~ENGINE_EFLAGS_CF) | saved_cf;
    return result;
}

static inline uint32_t engine_flags_dec(uint32_t *flags, uint32_t value,
                                        unsigned width_bits) {
    const uint32_t saved_cf = *flags & ENGINE_EFLAGS_CF;
    const uint32_t result = engine_flags_sub(flags, value, 1u, 0u, width_bits);
    *flags = (*flags & ~ENGINE_EFLAGS_CF) | saved_cf;
    return result;
}

static inline uint32_t engine_flags_neg(uint32_t *flags, uint32_t value,
                                        unsigned width_bits) {
    return engine_flags_sub(flags, 0u, value, 0u, width_bits);
}

static inline void engine_flags_shift_result(uint32_t *flags, uint32_t result,
                                             unsigned width_bits) {
    const uint32_t defined = ENGINE_EFLAGS_PF | ENGINE_EFLAGS_ZF |
                             ENGINE_EFLAGS_SF;
    const uint32_t bits = engine_flags_common(result, width_bits);
    /* AF is architecturally undefined after a nonzero shift; normalize it to 0. */
    *flags = (*flags & ~(defined | ENGINE_EFLAGS_AF)) | (bits & defined);
}

static inline uint32_t engine_flags_shl(uint32_t *flags, uint32_t value,
                                        uint32_t count,
                                        unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t input = value & mask;
    const uint32_t shift = count & UINT32_C(31);
    uint32_t result;
    uint32_t carry = 0u;
    if (shift == 0u) return input;
    result = shift >= width_bits ? 0u : (input << shift) & mask;
    engine_flags_shift_result(flags, result, width_bits);
    if (shift < width_bits) {
        carry = (input >> (width_bits - shift)) & UINT32_C(1);
        *flags = (*flags & ~ENGINE_EFLAGS_CF) |
                 (carry ? ENGINE_EFLAGS_CF : 0u);
    }
    /* CF is undefined when shift >= width; preserve it in that case. */
    if (shift == 1u) {
        const uint32_t overflow = ((result & sign) != 0u) ^ (carry != 0u);
        *flags = (*flags & ~ENGINE_EFLAGS_OF) |
                 (overflow ? ENGINE_EFLAGS_OF : 0u);
    }
    /* OF is undefined for shift > 1; preserve it. */
    return result;
}

static inline uint32_t engine_flags_shr(uint32_t *flags, uint32_t value,
                                        uint32_t count,
                                        unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t input = value & mask;
    const uint32_t shift = count & UINT32_C(31);
    uint32_t result;
    if (shift == 0u) return input;
    result = shift >= width_bits ? 0u : input >> shift;
    engine_flags_shift_result(flags, result, width_bits);
    if (shift < width_bits) {
        const uint32_t carry = (input >> (shift - 1u)) & UINT32_C(1);
        *flags = (*flags & ~ENGINE_EFLAGS_CF) |
                 (carry ? ENGINE_EFLAGS_CF : 0u);
    }
    /* CF is undefined when shift >= width; preserve it in that case. */
    if (shift == 1u) {
        *flags = (*flags & ~ENGINE_EFLAGS_OF) |
                 ((input & sign) ? ENGINE_EFLAGS_OF : 0u);
    }
    /* OF is undefined for shift > 1; preserve it. */
    return result;
}

static inline uint32_t engine_flags_sar(uint32_t *flags, uint32_t value,
                                        uint32_t count,
                                        unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t input = value & mask;
    const uint32_t shift = count & UINT32_C(31);
    uint32_t result;
    if (shift == 0u) return input;
    if (shift >= width_bits) {
        result = (input & sign) ? mask : 0u;
    } else {
        result = input >> shift;
        if ((input & sign) != 0u)
            result |= mask ^ (mask >> shift);
    }
    engine_flags_shift_result(flags, result, width_bits);
    if (shift < width_bits) {
        const uint32_t carry = (input >> (shift - 1u)) & UINT32_C(1);
        *flags = (*flags & ~ENGINE_EFLAGS_CF) |
                 (carry ? ENGINE_EFLAGS_CF : 0u);
    }
    /* CF is undefined when shift >= width; preserve it in that case. */
    if (shift == 1u)
        *flags &= ~ENGINE_EFLAGS_OF;
    /* OF is defined as zero for count 1 and undefined for count > 1. */
    return result;
}

static inline int64_t engine_flags_signed_value(uint32_t value,
                                                 unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const uint32_t sign = engine_flags_sign_mask(width_bits);
    const uint32_t narrowed = value & mask;
    if ((narrowed & sign) == 0u) return (int64_t)narrowed;
    return (int64_t)narrowed - (int64_t)((uint64_t)mask + UINT64_C(1));
}

static inline uint32_t engine_flags_imul(uint32_t *flags,
                                         uint32_t lhs, uint32_t rhs,
                                         unsigned width_bits) {
    const uint32_t mask = engine_flags_width_mask(width_bits);
    const int64_t product = engine_flags_signed_value(lhs, width_bits) *
                            engine_flags_signed_value(rhs, width_bits);
    const uint32_t result = (uint32_t)(uint64_t)product & mask;
    const uint32_t overflow =
        product != engine_flags_signed_value(result, width_bits);
    const uint32_t bits = overflow
        ? (ENGINE_EFLAGS_CF | ENGINE_EFLAGS_OF)
        : 0u;
    /* PF, AF, ZF, and SF are architecturally undefined for IMUL; preserve them. */
    *flags = (*flags & ~(ENGINE_EFLAGS_CF | ENGINE_EFLAGS_OF)) | bits;
    return result;
}

/* ---- Double-precision shifts and rotates (added for whole-executable translation). ---- */
static inline void engine_flags_set_szp(uint32_t *flags, uint32_t result, unsigned width_bits) {
    uint32_t mask = engine_flags_width_mask(width_bits);
    result &= mask;
    *flags &= ~(ENGINE_EFLAGS_SF | ENGINE_EFLAGS_ZF | ENGINE_EFLAGS_PF);
    if (result == 0u) *flags |= ENGINE_EFLAGS_ZF;
    if (result & engine_flags_sign_mask(width_bits)) *flags |= ENGINE_EFLAGS_SF;
    if (engine_flags_even_parity8(result)) *flags |= ENGINE_EFLAGS_PF;
}
static inline void engine_flags_set_cf_of(uint32_t *flags, uint32_t cf, uint32_t of) {
    *flags = (*flags & ~(ENGINE_EFLAGS_CF | ENGINE_EFLAGS_OF)) | (cf ? ENGINE_EFLAGS_CF : 0u) | (of ? ENGINE_EFLAGS_OF : 0u);
}
static inline uint32_t engine_flags_shld(uint32_t *flags, uint32_t dst, uint32_t src, uint32_t count, unsigned width_bits) {
    count &= 31u; if (count == 0u) return dst;
    uint32_t mask = engine_flags_width_mask(width_bits);
    dst &= mask; src &= mask;
    if (count > width_bits) count %= width_bits; /* architecturally undefined for 16-bit */
    uint64_t v = ((uint64_t)dst << width_bits) | src;
    uint32_t result = (uint32_t)((v << count) >> width_bits) & mask;
    engine_flags_set_cf_of(flags, (dst >> (width_bits - count)) & 1u, ((result ^ dst) >> (width_bits - 1u)) & 1u);
    engine_flags_set_szp(flags, result, width_bits);
    return result;
}
static inline uint32_t engine_flags_shrd(uint32_t *flags, uint32_t dst, uint32_t src, uint32_t count, unsigned width_bits) {
    count &= 31u; if (count == 0u) return dst;
    uint32_t mask = engine_flags_width_mask(width_bits);
    dst &= mask; src &= mask;
    if (count > width_bits) count %= width_bits;
    uint64_t v = ((uint64_t)src << width_bits) | dst;
    uint32_t result = (uint32_t)(v >> count) & mask;
    engine_flags_set_cf_of(flags, (dst >> (count - 1u)) & 1u, ((result ^ dst) >> (width_bits - 1u)) & 1u);
    engine_flags_set_szp(flags, result, width_bits);
    return result;
}
static inline uint32_t engine_flags_rol(uint32_t *flags, uint32_t value, uint32_t count, unsigned width_bits) {
    uint32_t mask = engine_flags_width_mask(width_bits);
    value &= mask; count &= 31u;
    if (count == 0u) return value;
    count %= width_bits;
    uint32_t result = count ? (((value << count) | (value >> (width_bits - count))) & mask) : value;
    uint32_t cf = result & 1u;
    engine_flags_set_cf_of(flags, cf, ((result >> (width_bits - 1u)) ^ cf) & 1u);
    return result;
}
static inline uint32_t engine_flags_ror(uint32_t *flags, uint32_t value, uint32_t count, unsigned width_bits) {
    uint32_t mask = engine_flags_width_mask(width_bits);
    value &= mask; count &= 31u;
    if (count == 0u) return value;
    count %= width_bits;
    uint32_t result = count ? (((value >> count) | (value << (width_bits - count))) & mask) : value;
    uint32_t msb = (result >> (width_bits - 1u)) & 1u;
    engine_flags_set_cf_of(flags, msb, (msb ^ (result >> (width_bits - 2u))) & 1u);
    return result;
}
static inline uint32_t engine_flags_rcl(uint32_t *flags, uint32_t value, uint32_t count, unsigned width_bits) {
    uint32_t mask = engine_flags_width_mask(width_bits);
    value &= mask; count = (count & 31u) % (width_bits + 1u);
    if (count == 0u) return value;
    uint32_t cf = *flags & ENGINE_EFLAGS_CF;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t new_cf = (value >> (width_bits - 1u)) & 1u;
        value = ((value << 1) | cf) & mask;
        cf = new_cf;
    }
    engine_flags_set_cf_of(flags, cf, (((value >> (width_bits - 1u)) & 1u) ^ cf) & 1u);
    return value;
}
static inline uint32_t engine_flags_rcr(uint32_t *flags, uint32_t value, uint32_t count, unsigned width_bits) {
    uint32_t mask = engine_flags_width_mask(width_bits);
    value &= mask; count = (count & 31u) % (width_bits + 1u);
    if (count == 0u) return value;
    uint32_t cf = *flags & ENGINE_EFLAGS_CF;
    uint32_t of = (((value >> (width_bits - 1u)) & 1u) ^ cf) & 1u;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t new_cf = value & 1u;
        value = (value >> 1) | (cf << (width_bits - 1u));
        cf = new_cf;
    }
    engine_flags_set_cf_of(flags, cf, of);
    return value;
}

#endif /* HALO_ENGINE_REUSE_ENGINE_FLAGS_H */
