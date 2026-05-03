

#ifndef LIB_DECOMPRESS_DEFS_H
#define LIB_DECOMPRESS_DEFS_H

#include "lib_common.h"

#include "deflate_constants.h"
#include "libdeflate.h"

#define PRECODE_TABLEBITS 7
#define PRECODE_ENOUGH 128
#define LITLEN_TABLEBITS 11
#define LITLEN_ENOUGH 2342
#define OFFSET_TABLEBITS 8
#define OFFSET_ENOUGH 402

#define HUFFDEC_LITERAL 0x80000000
#define HUFFDEC_EXCEPTIONAL 0x00008000
#define HUFFDEC_SUBTABLE_POINTER 0x00004000
#define HUFFDEC_END_OF_BLOCK 0x00002000

#define LENGTH_MAXBITS                                                         \
  (DEFLATE_MAX_LITLEN_CODEWORD_LEN + DEFLATE_MAX_EXTRA_LENGTH_BITS)
#define LENGTH_MAXFASTBITS (LITLEN_TABLEBITS + DEFLATE_MAX_EXTRA_LENGTH_BITS)

#define OFFSET_MAXBITS                                                         \
  (DEFLATE_MAX_OFFSET_CODEWORD_LEN + DEFLATE_MAX_EXTRA_OFFSET_BITS)
#define OFFSET_MAXFASTBITS (OFFSET_TABLEBITS + DEFLATE_MAX_EXTRA_OFFSET_BITS)

struct libdeflate_decompressor {

  union {
    u8 precode_lens[DEFLATE_NUM_PRECODE_SYMS];

    struct {
      u8 lens[DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS +
              DEFLATE_MAX_LENS_OVERRUN];

      u32 precode_decode_table[PRECODE_ENOUGH];
    } l;

    u32 litlen_decode_table[LITLEN_ENOUGH];
  } u;

  u32 offset_decode_table[OFFSET_ENOUGH];

  u16 sorted_syms[DEFLATE_MAX_NUM_SYMS];

  bool static_codes_loaded;
  unsigned litlen_tablebits;

  free_func_t free_func;
};

typedef enum libdeflate_result (*decompress_func_t)(
    struct libdeflate_decompressor *restrict d, const void *restrict in,
    size_t in_nbytes, void *restrict out, size_t out_nbytes_avail,
    size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

static forceinline u32 make_decode_table_entry(const u32 decode_results[],
                                               u32 sym, u32 len) {
  return decode_results[sym] + (len << 8) + len;
}

#ifdef LIBDEFLATE_DECOMPRESS_MAIN
#define DECOMP_INTERNAL static
#else
#define DECOMP_INTERNAL extern
#endif

DECOMP_INTERNAL MAYBE_UNUSED bool
build_precode_decode_table(struct libdeflate_decompressor *d);
DECOMP_INTERNAL MAYBE_UNUSED bool
build_litlen_decode_table(struct libdeflate_decompressor *d,
                          unsigned num_litlen_syms, unsigned num_offset_syms);
DECOMP_INTERNAL MAYBE_UNUSED bool
build_offset_decode_table(struct libdeflate_decompressor *d,
                          unsigned num_litlen_syms, unsigned num_offset_syms);

#endif
