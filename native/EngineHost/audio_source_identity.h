#ifndef HALO_AUDIO_SOURCE_IDENTITY_H
#define HALO_AUDIO_SOURCE_IDENTITY_H
#include <stddef.h>
#include <stdint.h>
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"
/* Diagnostic identity of every byte supplied to the decoder; not a sound tag. */
static inline uint64_t halo_audio_source_identity(const void *bytes,size_t size){return XXH3_64bits(bytes,size);}
#endif
