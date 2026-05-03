/*
 * deflate_decompress.c - a decompressor for DEFLATE
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  It is much faster than
 * vanilla zlib, typically well over twice as fast, though results vary by CPU.
 *
 * Why this is faster than vanilla zlib:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks
 * - Larger bitbuffer variable that doesn't need to be refilled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 * - On x86_64, a version of the decompression routine is compiled with BMI2
 *   instructions enabled and is used automatically at runtime when supported.
 */

#define LIBDEFLATE_DECOMPRESS_MAIN
#include "decompress_defs.h"

#if 0
#pragma message(                                                               \
    "UNSAFE DECOMPRESSION IS ENABLED. THIS MUST ONLY BE USED IF THE DECOMPRESSOR INPUT WILL ALWAYS BE TRUSTED!")
#define SAFETY_CHECK(expr) (void)(expr)
#else
#define SAFETY_CHECK(expr)                                                     \
  if (unlikely(!(expr)))                                                       \
  return LIBDEFLATE_BAD_DATA
#endif

#ifndef BITBUF_T_DEFINED
#define BITBUF_T_DEFINED
typedef machine_word_t bitbuf_t;
#endif
#define BITBUF_NBITS (8 * (int)sizeof(bitbuf_t))

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

#define PRECODE_TABLEBITS 7
#define PRECODE_ENOUGH 128
#define LITLEN_TABLEBITS 11
#define LITLEN_ENOUGH 2342
#define OFFSET_TABLEBITS 8
#define OFFSET_ENOUGH 402

static const u32 precode_decode_results[] = {
#define ENTRY(presym) ((u32)presym << 16)
    ENTRY(0),  ENTRY(1),  ENTRY(2),  ENTRY(3),  ENTRY(4),  ENTRY(5),  ENTRY(6),
    ENTRY(7),  ENTRY(8),  ENTRY(9),  ENTRY(10), ENTRY(11), ENTRY(12), ENTRY(13),
    ENTRY(14), ENTRY(15), ENTRY(16), ENTRY(17), ENTRY(18),
#undef ENTRY
};

#define HUFFDEC_LITERAL 0x80000000

#define HUFFDEC_EXCEPTIONAL 0x00008000

#define HUFFDEC_SUBTABLE_POINTER 0x00004000

#define HUFFDEC_END_OF_BLOCK 0x00002000

#define LENGTH_MAXBITS                                                         \
  (DEFLATE_MAX_LITLEN_CODEWORD_LEN + DEFLATE_MAX_EXTRA_LENGTH_BITS)
#define LENGTH_MAXFASTBITS (LITLEN_TABLEBITS + DEFLATE_MAX_EXTRA_LENGTH_BITS)

static const u32 litlen_decode_results[] = {

#define ENTRY(literal) (HUFFDEC_LITERAL | ((u32)literal << 16))
    ENTRY(0),
    ENTRY(1),
    ENTRY(2),
    ENTRY(3),
    ENTRY(4),
    ENTRY(5),
    ENTRY(6),
    ENTRY(7),
    ENTRY(8),
    ENTRY(9),
    ENTRY(10),
    ENTRY(11),
    ENTRY(12),
    ENTRY(13),
    ENTRY(14),
    ENTRY(15),
    ENTRY(16),
    ENTRY(17),
    ENTRY(18),
    ENTRY(19),
    ENTRY(20),
    ENTRY(21),
    ENTRY(22),
    ENTRY(23),
    ENTRY(24),
    ENTRY(25),
    ENTRY(26),
    ENTRY(27),
    ENTRY(28),
    ENTRY(29),
    ENTRY(30),
    ENTRY(31),
    ENTRY(32),
    ENTRY(33),
    ENTRY(34),
    ENTRY(35),
    ENTRY(36),
    ENTRY(37),
    ENTRY(38),
    ENTRY(39),
    ENTRY(40),
    ENTRY(41),
    ENTRY(42),
    ENTRY(43),
    ENTRY(44),
    ENTRY(45),
    ENTRY(46),
    ENTRY(47),
    ENTRY(48),
    ENTRY(49),
    ENTRY(50),
    ENTRY(51),
    ENTRY(52),
    ENTRY(53),
    ENTRY(54),
    ENTRY(55),
    ENTRY(56),
    ENTRY(57),
    ENTRY(58),
    ENTRY(59),
    ENTRY(60),
    ENTRY(61),
    ENTRY(62),
    ENTRY(63),
    ENTRY(64),
    ENTRY(65),
    ENTRY(66),
    ENTRY(67),
    ENTRY(68),
    ENTRY(69),
    ENTRY(70),
    ENTRY(71),
    ENTRY(72),
    ENTRY(73),
    ENTRY(74),
    ENTRY(75),
    ENTRY(76),
    ENTRY(77),
    ENTRY(78),
    ENTRY(79),
    ENTRY(80),
    ENTRY(81),
    ENTRY(82),
    ENTRY(83),
    ENTRY(84),
    ENTRY(85),
    ENTRY(86),
    ENTRY(87),
    ENTRY(88),
    ENTRY(89),
    ENTRY(90),
    ENTRY(91),
    ENTRY(92),
    ENTRY(93),
    ENTRY(94),
    ENTRY(95),
    ENTRY(96),
    ENTRY(97),
    ENTRY(98),
    ENTRY(99),
    ENTRY(100),
    ENTRY(101),
    ENTRY(102),
    ENTRY(103),
    ENTRY(104),
    ENTRY(105),
    ENTRY(106),
    ENTRY(107),
    ENTRY(108),
    ENTRY(109),
    ENTRY(110),
    ENTRY(111),
    ENTRY(112),
    ENTRY(113),
    ENTRY(114),
    ENTRY(115),
    ENTRY(116),
    ENTRY(117),
    ENTRY(118),
    ENTRY(119),
    ENTRY(120),
    ENTRY(121),
    ENTRY(122),
    ENTRY(123),
    ENTRY(124),
    ENTRY(125),
    ENTRY(126),
    ENTRY(127),
    ENTRY(128),
    ENTRY(129),
    ENTRY(130),
    ENTRY(131),
    ENTRY(132),
    ENTRY(133),
    ENTRY(134),
    ENTRY(135),
    ENTRY(136),
    ENTRY(137),
    ENTRY(138),
    ENTRY(139),
    ENTRY(140),
    ENTRY(141),
    ENTRY(142),
    ENTRY(143),
    ENTRY(144),
    ENTRY(145),
    ENTRY(146),
    ENTRY(147),
    ENTRY(148),
    ENTRY(149),
    ENTRY(150),
    ENTRY(151),
    ENTRY(152),
    ENTRY(153),
    ENTRY(154),
    ENTRY(155),
    ENTRY(156),
    ENTRY(157),
    ENTRY(158),
    ENTRY(159),
    ENTRY(160),
    ENTRY(161),
    ENTRY(162),
    ENTRY(163),
    ENTRY(164),
    ENTRY(165),
    ENTRY(166),
    ENTRY(167),
    ENTRY(168),
    ENTRY(169),
    ENTRY(170),
    ENTRY(171),
    ENTRY(172),
    ENTRY(173),
    ENTRY(174),
    ENTRY(175),
    ENTRY(176),
    ENTRY(177),
    ENTRY(178),
    ENTRY(179),
    ENTRY(180),
    ENTRY(181),
    ENTRY(182),
    ENTRY(183),
    ENTRY(184),
    ENTRY(185),
    ENTRY(186),
    ENTRY(187),
    ENTRY(188),
    ENTRY(189),
    ENTRY(190),
    ENTRY(191),
    ENTRY(192),
    ENTRY(193),
    ENTRY(194),
    ENTRY(195),
    ENTRY(196),
    ENTRY(197),
    ENTRY(198),
    ENTRY(199),
    ENTRY(200),
    ENTRY(201),
    ENTRY(202),
    ENTRY(203),
    ENTRY(204),
    ENTRY(205),
    ENTRY(206),
    ENTRY(207),
    ENTRY(208),
    ENTRY(209),
    ENTRY(210),
    ENTRY(211),
    ENTRY(212),
    ENTRY(213),
    ENTRY(214),
    ENTRY(215),
    ENTRY(216),
    ENTRY(217),
    ENTRY(218),
    ENTRY(219),
    ENTRY(220),
    ENTRY(221),
    ENTRY(222),
    ENTRY(223),
    ENTRY(224),
    ENTRY(225),
    ENTRY(226),
    ENTRY(227),
    ENTRY(228),
    ENTRY(229),
    ENTRY(230),
    ENTRY(231),
    ENTRY(232),
    ENTRY(233),
    ENTRY(234),
    ENTRY(235),
    ENTRY(236),
    ENTRY(237),
    ENTRY(238),
    ENTRY(239),
    ENTRY(240),
    ENTRY(241),
    ENTRY(242),
    ENTRY(243),
    ENTRY(244),
    ENTRY(245),
    ENTRY(246),
    ENTRY(247),
    ENTRY(248),
    ENTRY(249),
    ENTRY(250),
    ENTRY(251),
    ENTRY(252),
    ENTRY(253),
    ENTRY(254),
    ENTRY(255),
#undef ENTRY

    HUFFDEC_EXCEPTIONAL | HUFFDEC_END_OF_BLOCK,

#define ENTRY(length_base, num_extra_bits)                                     \
  (((u32)(length_base) << 16) | (num_extra_bits))
    ENTRY(3, 0),
    ENTRY(4, 0),
    ENTRY(5, 0),
    ENTRY(6, 0),
    ENTRY(7, 0),
    ENTRY(8, 0),
    ENTRY(9, 0),
    ENTRY(10, 0),
    ENTRY(11, 1),
    ENTRY(13, 1),
    ENTRY(15, 1),
    ENTRY(17, 1),
    ENTRY(19, 2),
    ENTRY(23, 2),
    ENTRY(27, 2),
    ENTRY(31, 2),
    ENTRY(35, 3),
    ENTRY(43, 3),
    ENTRY(51, 3),
    ENTRY(59, 3),
    ENTRY(67, 4),
    ENTRY(83, 4),
    ENTRY(99, 4),
    ENTRY(115, 4),
    ENTRY(131, 5),
    ENTRY(163, 5),
    ENTRY(195, 5),
    ENTRY(227, 5),
    ENTRY(258, 0),
    ENTRY(258, 0),
    ENTRY(258, 0),
#undef ENTRY
};

#define OFFSET_MAXBITS                                                         \
  (DEFLATE_MAX_OFFSET_CODEWORD_LEN + DEFLATE_MAX_EXTRA_OFFSET_BITS)
#define OFFSET_MAXFASTBITS (OFFSET_TABLEBITS + DEFLATE_MAX_EXTRA_OFFSET_BITS)

static const u32 offset_decode_results[] = {
#define ENTRY(offset_base, num_extra_bits)                                     \
  (((u32)(offset_base) << 16) | (num_extra_bits))
    ENTRY(1, 0),      ENTRY(2, 0),      ENTRY(3, 0),      ENTRY(4, 0),
    ENTRY(5, 1),      ENTRY(7, 1),      ENTRY(9, 2),      ENTRY(13, 2),
    ENTRY(17, 3),     ENTRY(25, 3),     ENTRY(33, 4),     ENTRY(49, 4),
    ENTRY(65, 5),     ENTRY(97, 5),     ENTRY(129, 6),    ENTRY(193, 6),
    ENTRY(257, 7),    ENTRY(385, 7),    ENTRY(513, 8),    ENTRY(769, 8),
    ENTRY(1025, 9),   ENTRY(1537, 9),   ENTRY(2049, 10),  ENTRY(3073, 10),
    ENTRY(4097, 11),  ENTRY(6145, 11),  ENTRY(8193, 12),  ENTRY(12289, 12),
    ENTRY(16385, 13), ENTRY(24577, 13), ENTRY(24577, 13), ENTRY(24577, 13),
#undef ENTRY
};

static bool build_decode_table(u32 decode_table[], const u8 lens[],
                               const unsigned num_syms,
                               const u32 decode_results[], unsigned table_bits,
                               unsigned max_codeword_len, u16 *sorted_syms,
                               unsigned *table_bits_ret) {
  unsigned len_counts[DEFLATE_MAX_CODEWORD_LEN + 1];
  unsigned offsets[DEFLATE_MAX_CODEWORD_LEN + 1];
  unsigned sym;
  unsigned codeword;
  unsigned len;
  unsigned count;
  u32 codespace_used;
  unsigned cur_table_end;
  unsigned subtable_prefix;
  unsigned subtable_start;
  unsigned subtable_bits;

  for (len = 0; len <= max_codeword_len; len++)
    len_counts[len] = 0;
  for (sym = 0; sym < num_syms; sym++)
    len_counts[lens[sym]]++;

  while (max_codeword_len > 1 && len_counts[max_codeword_len] == 0)
    max_codeword_len--;
  if (table_bits_ret != NULL) {
    table_bits = MIN(table_bits, max_codeword_len);
    *table_bits_ret = table_bits;
  }

  STATIC_ASSERT(sizeof(codespace_used) == 4);
  STATIC_ASSERT(UINT32_MAX / (1U << (DEFLATE_MAX_CODEWORD_LEN - 1)) >=
                DEFLATE_MAX_NUM_SYMS);

  offsets[0] = 0;
  offsets[1] = len_counts[0];
  codespace_used = 0;
  for (len = 1; len < max_codeword_len; len++) {
    offsets[len + 1] = offsets[len] + len_counts[len];
    codespace_used = (codespace_used << 1) + len_counts[len];
  }
  codespace_used = (codespace_used << 1) + len_counts[len];

  for (sym = 0; sym < num_syms; sym++)
    sorted_syms[offsets[lens[sym]]++] = sym;

  sorted_syms += offsets[0];

  if (unlikely(codespace_used > (1U << max_codeword_len)))
    return false;

  if (unlikely(codespace_used < (1U << max_codeword_len))) {
    u32 entry;
    unsigned i;

    if (codespace_used == 0) {
      sym = 0;
    } else {
      if (codespace_used != (1U << (max_codeword_len - 1)) ||
          len_counts[1] != 1)
        return false;
      sym = sorted_syms[0];
    }
    entry = make_decode_table_entry(decode_results, sym, 1);
    for (i = 0; i < (1U << table_bits); i++)
      decode_table[i] = entry;
    return true;
  }

  codeword = 0;
  len = 1;
  while ((count = len_counts[len]) == 0)
    len++;
  cur_table_end = 1U << len;
  while (len <= table_bits) {

    do {
      unsigned bit;

      decode_table[codeword] =
          make_decode_table_entry(decode_results, *sorted_syms++, len);

      if (codeword == cur_table_end - 1) {

        for (; len < table_bits; len++) {
          memcpy(&decode_table[cur_table_end], decode_table,
                 cur_table_end * sizeof(decode_table[0]));
          cur_table_end <<= 1;
        }
        return true;
      }

      bit = 1U << bsr32(codeword ^ (cur_table_end - 1));
      codeword &= bit - 1;
      codeword |= bit;
    } while (--count);

    do {
      if (++len <= table_bits) {
        memcpy(&decode_table[cur_table_end], decode_table,
               cur_table_end * sizeof(decode_table[0]));
        cur_table_end <<= 1;
      }
    } while ((count = len_counts[len]) == 0);
  }

  cur_table_end = 1U << table_bits;
  subtable_prefix = -1;
  subtable_start = 0;
  for (;;) {
    u32 entry;
    unsigned i;
    unsigned stride;
    unsigned bit;

    if ((codeword & ((1U << table_bits) - 1)) != subtable_prefix) {
      subtable_prefix = (codeword & ((1U << table_bits) - 1));
      subtable_start = cur_table_end;

      subtable_bits = len - table_bits;
      codespace_used = count;
      while (codespace_used < (1U << subtable_bits)) {
        subtable_bits++;
        codespace_used =
            (codespace_used << 1) + len_counts[table_bits + subtable_bits];
      }
      cur_table_end = subtable_start + (1U << subtable_bits);

      decode_table[subtable_prefix] =
          ((u32)subtable_start << 16) | HUFFDEC_EXCEPTIONAL |
          HUFFDEC_SUBTABLE_POINTER | (subtable_bits << 8) | table_bits;
    }

    entry = make_decode_table_entry(decode_results, *sorted_syms++,
                                    len - table_bits);
    i = subtable_start + (codeword >> table_bits);
    stride = 1U << (len - table_bits);
    do {
      decode_table[i] = entry;
      i += stride;
    } while (i < cur_table_end);

    if (codeword == (1U << len) - 1)
      return true;
    bit = 1U << bsr32(codeword ^ ((1U << len) - 1));
    codeword &= bit - 1;
    codeword |= bit;
    count--;
    while (count == 0)
      count = len_counts[++len];
  }
}

DECOMP_INTERNAL MAYBE_UNUSED bool
build_precode_decode_table(struct libdeflate_decompressor *d) {

  STATIC_ASSERT(PRECODE_TABLEBITS == 7 && PRECODE_ENOUGH == 128);

  STATIC_ASSERT(ARRAY_LEN(precode_decode_results) == DEFLATE_NUM_PRECODE_SYMS);

  return build_decode_table(d->u.l.precode_decode_table, d->u.precode_lens,
                            DEFLATE_NUM_PRECODE_SYMS, precode_decode_results,
                            PRECODE_TABLEBITS, DEFLATE_MAX_PRE_CODEWORD_LEN,
                            d->sorted_syms, NULL);
}

DECOMP_INTERNAL MAYBE_UNUSED bool
build_litlen_decode_table(struct libdeflate_decompressor *d,
                          unsigned num_litlen_syms, unsigned num_offset_syms) {

  STATIC_ASSERT(LITLEN_TABLEBITS == 11 && LITLEN_ENOUGH == 2342);

  STATIC_ASSERT(ARRAY_LEN(litlen_decode_results) == DEFLATE_NUM_LITLEN_SYMS);

  return build_decode_table(d->u.litlen_decode_table, d->u.l.lens,
                            num_litlen_syms, litlen_decode_results,
                            LITLEN_TABLEBITS, DEFLATE_MAX_LITLEN_CODEWORD_LEN,
                            d->sorted_syms, &d->litlen_tablebits);
}

DECOMP_INTERNAL MAYBE_UNUSED bool
build_offset_decode_table(struct libdeflate_decompressor *d,
                          unsigned num_litlen_syms, unsigned num_offset_syms) {

  STATIC_ASSERT(OFFSET_TABLEBITS == 8 && OFFSET_ENOUGH == 402);

  STATIC_ASSERT(ARRAY_LEN(offset_decode_results) == DEFLATE_NUM_OFFSET_SYMS);

  return build_decode_table(
      d->offset_decode_table, d->u.l.lens + num_litlen_syms, num_offset_syms,
      offset_decode_results, OFFSET_TABLEBITS, DEFLATE_MAX_OFFSET_CODEWORD_LEN,
      d->sorted_syms, NULL);
}

#define FUNCNAME deflate_decompress_default
#undef ATTRIBUTES
#undef EXTRACT_VARBITS
#undef EXTRACT_VARBITS8
#include "decompress_template.h"

#undef DEFAULT_IMPL
#undef LIBDEFLATE_HAVE_ARCH_SELECT_DECOMPRESS_FUNC
#if defined(ARCH_X86_32) || defined(ARCH_X86_64)
#include "decompress_impl.h"
#endif

#ifndef DEFAULT_IMPL
#define DEFAULT_IMPL deflate_decompress_default
#endif

#ifdef LIBDEFLATE_HAVE_ARCH_SELECT_DECOMPRESS_FUNC
static enum libdeflate_result
dispatch_decomp(struct libdeflate_decompressor *d, const void *in,
                size_t in_nbytes, void *out, size_t out_nbytes_avail,
                size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

static volatile decompress_func_t decompress_impl = dispatch_decomp;

static enum libdeflate_result
dispatch_decomp(struct libdeflate_decompressor *d, const void *in,
                size_t in_nbytes, void *out, size_t out_nbytes_avail,
                size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret) {
  decompress_func_t f = arch_select_decompress_func();

  if (f == NULL)
    f = DEFAULT_IMPL;

  decompress_impl = f;
  return f(d, in, in_nbytes, out, out_nbytes_avail, actual_in_nbytes_ret,
           actual_out_nbytes_ret);
}
#else

#define decompress_impl DEFAULT_IMPL
#endif

LIBDEFLATEAPI enum libdeflate_result libdeflate_deflate_decompress_ex(
    struct libdeflate_decompressor *d, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail, size_t *actual_in_nbytes_ret,
    size_t *actual_out_nbytes_ret) {
  return decompress_impl(d, in, in_nbytes, out, out_nbytes_avail,
                         actual_in_nbytes_ret, actual_out_nbytes_ret);
}

LIBDEFLATEAPI enum libdeflate_result libdeflate_deflate_decompress(
    struct libdeflate_decompressor *d, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail, size_t *actual_out_nbytes_ret) {
  return libdeflate_deflate_decompress_ex(
      d, in, in_nbytes, out, out_nbytes_avail, NULL, actual_out_nbytes_ret);
}

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor_ex(const struct libdeflate_options *options) {
  struct libdeflate_decompressor *d;

  if (options->sizeof_options != sizeof(*options))
    return NULL;

  d = (options->malloc_func ? options->malloc_func
                            : libdeflate_default_malloc_func)(sizeof(*d));
  if (d == NULL)
    return NULL;

  memset(d, 0, sizeof(*d));
  d->free_func =
      options->free_func ? options->free_func : libdeflate_default_free_func;
  return d;
}

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void) {
  static const struct libdeflate_options defaults = {
      .sizeof_options = sizeof(defaults),
  };
  return libdeflate_alloc_decompressor_ex(&defaults);
}

LIBDEFLATEAPI void
libdeflate_free_decompressor(struct libdeflate_decompressor *d) {
  if (d)
    d->free_func(d);
}
