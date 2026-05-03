

#ifndef LIB_MATCHFINDER_DEFS_H
#define LIB_MATCHFINDER_DEFS_H

#include "common_defs.h"

#ifndef MATCHFINDER_WINDOW_ORDER
#define MATCHFINDER_WINDOW_ORDER 15
#endif

#define MATCHFINDER_WINDOW_SIZE (1UL << MATCHFINDER_WINDOW_ORDER)

static forceinline u32 loaded_u32_to_u24(u32 v) {
  if (CPU_IS_LITTLE_ENDIAN())
    return v & 0xFFFFFF;
  else
    return v >> 8;
}

static forceinline u32 load_u24_unaligned(const u8 *p) {
#if UNALIGNED_ACCESS_IS_FAST
  return loaded_u32_to_u24(load_u32_unaligned(p));
#else
  if (CPU_IS_LITTLE_ENDIAN())
    return ((u32)p[0] << 0) | ((u32)p[1] << 8) | ((u32)p[2] << 16);
  else
    return ((u32)p[2] << 0) | ((u32)p[1] << 8) | ((u32)p[0] << 16);
#endif
}

typedef s16 mf_pos_t;

#define MATCHFINDER_INITVAL ((mf_pos_t) - MATCHFINDER_WINDOW_SIZE)

#define MATCHFINDER_MEM_ALIGNMENT 32

#define MATCHFINDER_SIZE_ALIGNMENT 1024

#endif
