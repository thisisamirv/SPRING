/*
 * matchfinder_defs.h - shared types and constants for matchfinding
 */

#ifndef LIB_MATCHFINDER_DEFS_H
#define LIB_MATCHFINDER_DEFS_H

#include "../common_defs.h"

#ifndef MATCHFINDER_WINDOW_ORDER
#define MATCHFINDER_WINDOW_ORDER 15
#endif

#define MATCHFINDER_WINDOW_SIZE (1UL << MATCHFINDER_WINDOW_ORDER)

/*
 * Given a 32-bit value that was loaded with the platform's native endianness,
 * return a 32-bit value whose high-order 8 bits are 0 and whose low-order 24
 * bits contain the first 3 bytes, arranged in octets in a platform-dependent
 * order, at the memory location from which the input 32-bit value was loaded.
 */
static forceinline u32 loaded_u32_to_u24(u32 v) {
  if (CPU_IS_LITTLE_ENDIAN())
    return v & 0xFFFFFF;
  else
    return v >> 8;
}

/*
 * Load the next 3 bytes from @p into the 24 low-order bits of a 32-bit value.
 * The order in which the 3 bytes will be arranged as octets in the 24 bits is
 * platform-dependent.  At least 4 bytes (not 3) must be available at @p.
 */
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

/*
 * This is the memory address alignment, in bytes, required for the matchfinder
 * buffers by the architecture-specific implementations of matchfinder_init()
 * and matchfinder_rebase().
 */
#define MATCHFINDER_MEM_ALIGNMENT 32

/*
 * This declares a size, in bytes, that is guaranteed to divide the sizes of the
 * matchfinder buffers.
 */
#define MATCHFINDER_SIZE_ALIGNMENT 1024

#endif /* LIB_MATCHFINDER_DEFS_H */
