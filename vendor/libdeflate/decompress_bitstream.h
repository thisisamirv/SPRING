/*
 * decompress_bitstream.h - decompression bitstream handling
 */

#ifndef LIB_DECOMPRESS_BITSTREAM_H
#define LIB_DECOMPRESS_BITSTREAM_H

#include "common_defs.h"
#include "decompress_defs.h"
#include "deflate_constants.h"

/*
 * If the expression passed to SAFETY_CHECK() evaluates to false, then the
 * decompression routine immediately returns LIBDEFLATE_BAD_DATA, indicating the
 * compressed data is invalid.
 */
#if 0
#define SAFETY_CHECK(expr) (void)(expr)
#else
#define SAFETY_CHECK(expr)                                                     \
  if (unlikely(!(expr)))                                                       \
  return LIBDEFLATE_BAD_DATA
#endif

/*
 * The type for the bitbuffer variable.  For best performance, this should have
 * size equal to a machine word.
 */
#ifndef BITBUF_T_DEFINED
#define BITBUF_T_DEFINED
typedef machine_word_t bitbuf_t;
#endif
#define BITBUF_NBITS (8 * (int)sizeof(bitbuf_t))

/* BITMASK(n) returns a bitmask of length 'n'. */
#define BITMASK(n) (((bitbuf_t)1 << (n)) - 1)

#define MAX_BITSLEFT                                                           \
  (UNALIGNED_ACCESS_IS_FAST ? BITBUF_NBITS - 1 : BITBUF_NBITS)

#define CONSUMABLE_NBITS (MAX_BITSLEFT - 7)

#define FASTLOOP_PRELOADABLE_NBITS                                             \
  (UNALIGNED_ACCESS_IS_FAST ? BITBUF_NBITS : CONSUMABLE_NBITS)

#define PRELOAD_SLACK MAX(0, FASTLOOP_PRELOADABLE_NBITS - MAX_BITSLEFT)

#define CAN_CONSUME(n) (CONSUMABLE_NBITS >= (n))

#define CAN_CONSUME_AND_THEN_PRELOAD(consume_nbits, preload_nbits)             \
  (CONSUMABLE_NBITS >= (consume_nbits) &&                                      \
   FASTLOOP_PRELOADABLE_NBITS >= (consume_nbits) + (preload_nbits))

#define REFILL_BITS_BRANCHLESS()                                               \
  do {                                                                         \
    bitbuf |= get_unaligned_leword(in_next) << (u8)bitsleft;                   \
    in_next += sizeof(bitbuf_t) - 1;                                           \
    in_next -= (bitsleft >> 3) & 0x7;                                          \
    bitsleft |= MAX_BITSLEFT & ~7;                                             \
  } while (0)

#define REFILL_BITS()                                                          \
  do {                                                                         \
    if (UNALIGNED_ACCESS_IS_FAST &&                                            \
        likely(in_end - in_next >= sizeof(bitbuf_t))) {                        \
      REFILL_BITS_BRANCHLESS();                                                \
    } else {                                                                   \
      while ((u8)bitsleft < CONSUMABLE_NBITS) {                                \
        if (likely(in_next != in_end)) {                                       \
          bitbuf |= (bitbuf_t) * in_next++ << (u8)bitsleft;                    \
        } else {                                                               \
          overread_count++;                                                    \
          SAFETY_CHECK(overread_count <= sizeof(bitbuf_t));                    \
        }                                                                      \
        bitsleft += 8;                                                         \
      }                                                                        \
    }                                                                          \
  } while (0)

#define REFILL_BITS_IN_FASTLOOP()                                              \
  do {                                                                         \
    STATIC_ASSERT(UNALIGNED_ACCESS_IS_FAST ||                                  \
                  FASTLOOP_PRELOADABLE_NBITS == CONSUMABLE_NBITS);             \
    if (UNALIGNED_ACCESS_IS_FAST) {                                            \
      REFILL_BITS_BRANCHLESS();                                                \
    } else {                                                                   \
      while ((u8)bitsleft < CONSUMABLE_NBITS) {                                \
        bitbuf |= (bitbuf_t) * in_next++ << (u8)bitsleft;                      \
        bitsleft += 8;                                                         \
      }                                                                        \
    }                                                                          \
  } while (0)

#define FASTLOOP_MAX_BYTES_WRITTEN                                             \
  (2 + DEFLATE_MAX_MATCH_LEN + (5 * WORDBYTES) - 1)

#define FASTLOOP_MAX_BYTES_READ                                                \
  (DIV_ROUND_UP(MAX_BITSLEFT + (2 * LITLEN_TABLEBITS) + LENGTH_MAXBITS +       \
                    OFFSET_MAXBITS,                                            \
                8) +                                                           \
   sizeof(bitbuf_t))

#endif /* LIB_DECOMPRESS_BITSTREAM_H */
