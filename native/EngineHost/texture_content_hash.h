#ifndef HALO_TEXTURE_CONTENT_HASH_H
#define HALO_TEXTURE_CONTENT_HASH_H

/* Texture cache keys are private to a renderer lifetime. Keep hashing every
 * source byte, including GPU readbacks, rather than assuming guest memory is
 * unchanged between draws. XXH3 removes the serial per-byte FNV multiply from
 * this hot path without introducing resource dirty-tracking assumptions.
 * Upstream v0.8.3 and its BSD license are in third_party/xxhash/. */
#include <stddef.h>
#include <stdint.h>
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

static inline uint64_t halo_texture_hash_more(uint64_t seed, const void *bytes, size_t size) {
    return XXH3_64bits_withSeed(bytes, size, seed);
}

#endif
