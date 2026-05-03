/*
 * decompress_template.h
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
 */

#include "decompress_bitstream.h"

#ifndef ATTRIBUTES
#define ATTRIBUTES
#endif
#ifndef EXTRACT_VARBITS
#define EXTRACT_VARBITS(word, count) ((word) & BITMASK(count))
#endif
#ifndef EXTRACT_VARBITS8
#define EXTRACT_VARBITS8(word, count) ((word) & BITMASK((u8)(count)))
#endif

static ATTRIBUTES MAYBE_UNUSED enum libdeflate_result
FUNCNAME(struct libdeflate_decompressor *restrict d, const void *restrict in,
         size_t in_nbytes, void *restrict out, size_t out_nbytes_avail,
         size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret) {
  u8 *out_next = (u8 *)out;
  u8 *const out_end = out_next + out_nbytes_avail;
  u8 *const out_fastloop_end =
      out_end - MIN(out_nbytes_avail, FASTLOOP_MAX_BYTES_WRITTEN);

  const u8 *in_next = (const u8 *)in;
  const u8 *const in_end = in_next + in_nbytes;
  const u8 *const in_fastloop_end =
      in_end - MIN(in_nbytes, FASTLOOP_MAX_BYTES_READ);
  bitbuf_t bitbuf = 0;
  bitbuf_t saved_bitbuf;
  u32 bitsleft = 0;
  size_t overread_count = 0;

  bool is_final_block;
  unsigned block_type;
  unsigned num_litlen_syms;
  unsigned num_offset_syms;
  bitbuf_t litlen_tablemask;
  u32 entry;

next_block:

    ;

  STATIC_ASSERT(CAN_CONSUME(1 + 2 + 5 + 5 + 4 + 3));
  REFILL_BITS();

  is_final_block = bitbuf & BITMASK(1);

  block_type = (bitbuf >> 1) & BITMASK(2);

  if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

    static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] =
        {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    unsigned num_explicit_precode_lens;
    unsigned i;

    STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 257 + BITMASK(5));
    num_litlen_syms = 257 + ((bitbuf >> 3) & BITMASK(5));

    STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 1 + BITMASK(5));
    num_offset_syms = 1 + ((bitbuf >> 8) & BITMASK(5));

    STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == 4 + BITMASK(4));
    num_explicit_precode_lens = 4 + ((bitbuf >> 13) & BITMASK(4));

    d->static_codes_loaded = false;

    STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
    if (CAN_CONSUME(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {
      d->u.precode_lens[deflate_precode_lens_permutation[0]] =
          (bitbuf >> 17) & BITMASK(3);
      bitbuf >>= 20;
      bitsleft -= 20;
      REFILL_BITS();
      i = 1;
      do {
        d->u.precode_lens[deflate_precode_lens_permutation[i]] =
            bitbuf & BITMASK(3);
        bitbuf >>= 3;
        bitsleft -= 3;
      } while (++i < num_explicit_precode_lens);
    } else {
      bitbuf >>= 17;
      bitsleft -= 17;
      i = 0;
      do {
        if ((u8)bitsleft < 3)
          REFILL_BITS();
        d->u.precode_lens[deflate_precode_lens_permutation[i]] =
            bitbuf & BITMASK(3);
        bitbuf >>= 3;
        bitsleft -= 3;
      } while (++i < num_explicit_precode_lens);
    }
    for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
      d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

    SAFETY_CHECK(build_precode_decode_table(d));

    i = 0;
    do {
      unsigned presym;
      u8 rep_val;
      unsigned rep_count;

      if ((u8)bitsleft < DEFLATE_MAX_PRE_CODEWORD_LEN + 7)
        REFILL_BITS();

      STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

      entry =
          d->u.l.precode_decode_table[bitbuf &
                                      BITMASK(DEFLATE_MAX_PRE_CODEWORD_LEN)];
      bitbuf >>= (u8)entry;
      bitsleft -= entry;
      presym = entry >> 16;

      if (presym < 16) {

        d->u.l.lens[i++] = presym;
        continue;
      }

      STATIC_ASSERT(DEFLATE_MAX_LENS_OVERRUN == 138 - 1);

      if (presym == 16) {

        SAFETY_CHECK(i != 0);
        rep_val = d->u.l.lens[i - 1];
        STATIC_ASSERT(3 + BITMASK(2) == 6);
        rep_count = 3 + (bitbuf & BITMASK(2));
        bitbuf >>= 2;
        bitsleft -= 2;
        d->u.l.lens[i + 0] = rep_val;
        d->u.l.lens[i + 1] = rep_val;
        d->u.l.lens[i + 2] = rep_val;
        d->u.l.lens[i + 3] = rep_val;
        d->u.l.lens[i + 4] = rep_val;
        d->u.l.lens[i + 5] = rep_val;
        i += rep_count;
      } else if (presym == 17) {

        STATIC_ASSERT(3 + BITMASK(3) == 10);
        rep_count = 3 + (bitbuf & BITMASK(3));
        bitbuf >>= 3;
        bitsleft -= 3;
        d->u.l.lens[i + 0] = 0;
        d->u.l.lens[i + 1] = 0;
        d->u.l.lens[i + 2] = 0;
        d->u.l.lens[i + 3] = 0;
        d->u.l.lens[i + 4] = 0;
        d->u.l.lens[i + 5] = 0;
        d->u.l.lens[i + 6] = 0;
        d->u.l.lens[i + 7] = 0;
        d->u.l.lens[i + 8] = 0;
        d->u.l.lens[i + 9] = 0;
        i += rep_count;
      } else {

        STATIC_ASSERT(11 + BITMASK(7) == 138);
        rep_count = 11 + (bitbuf & BITMASK(7));
        bitbuf >>= 7;
        bitsleft -= 7;
        memset(&d->u.l.lens[i], 0, rep_count * sizeof(d->u.l.lens[i]));
        i += rep_count;
      }
    } while (i < num_litlen_syms + num_offset_syms);

    SAFETY_CHECK(i == num_litlen_syms + num_offset_syms);

  } else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
    u16 len, nlen;

    bitsleft -= 3;

    bitsleft = (u8)bitsleft;
    SAFETY_CHECK(overread_count <= (bitsleft >> 3));
    in_next -= (bitsleft >> 3) - overread_count;
    overread_count = 0;
    bitbuf = 0;
    bitsleft = 0;

    SAFETY_CHECK(in_end - in_next >= 4);
    len = get_unaligned_le16(in_next);
    nlen = get_unaligned_le16(in_next + 2);
    in_next += 4;

    SAFETY_CHECK(len == (u16)~nlen);
    if (unlikely(len > out_end - out_next))
      return LIBDEFLATE_INSUFFICIENT_SPACE;
    SAFETY_CHECK(len <= in_end - in_next);

    memcpy(out_next, in_next, len);
    in_next += len;
    out_next += len;

    goto block_done;

  } else {
    unsigned i;

    SAFETY_CHECK(block_type == DEFLATE_BLOCKTYPE_STATIC_HUFFMAN);

    bitbuf >>= 3;
    bitsleft -= 3;

    if (d->static_codes_loaded)
      goto have_decode_tables;

    d->static_codes_loaded = true;

    STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 288);
    STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 32);

    for (i = 0; i < 144; i++)
      d->u.l.lens[i] = 8;
    for (; i < 256; i++)
      d->u.l.lens[i] = 9;
    for (; i < 280; i++)
      d->u.l.lens[i] = 7;
    for (; i < 288; i++)
      d->u.l.lens[i] = 8;

    for (; i < 288 + 32; i++)
      d->u.l.lens[i] = 5;

    num_litlen_syms = 288;
    num_offset_syms = 32;
  }

  SAFETY_CHECK(build_offset_decode_table(d, num_litlen_syms, num_offset_syms));
  SAFETY_CHECK(build_litlen_decode_table(d, num_litlen_syms, num_offset_syms));
have_decode_tables:
  litlen_tablemask = BITMASK(d->litlen_tablebits);

  if (in_next >= in_fastloop_end || out_next >= out_fastloop_end)
    goto generic_loop;
  REFILL_BITS_IN_FASTLOOP();
  entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
  do {
    u32 length, offset, lit;
    const u8 *src;
    u8 *dst;

    saved_bitbuf = bitbuf;
    bitbuf >>= (u8)entry;
    bitsleft -= entry;

    if (entry & HUFFDEC_LITERAL) {

      if (CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS + LENGTH_MAXBITS,
                                       OFFSET_TABLEBITS) &&

          CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
                                           DEFLATE_MAX_LITLEN_CODEWORD_LEN,
                                       LITLEN_TABLEBITS)) {

        lit = entry >> 16;
        entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
        saved_bitbuf = bitbuf;
        bitbuf >>= (u8)entry;
        bitsleft -= entry;
        *out_next++ = lit;
        if (entry & HUFFDEC_LITERAL) {

          lit = entry >> 16;
          entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
          saved_bitbuf = bitbuf;
          bitbuf >>= (u8)entry;
          bitsleft -= entry;
          *out_next++ = lit;
          if (entry & HUFFDEC_LITERAL) {

            lit = entry >> 16;
            entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
            REFILL_BITS_IN_FASTLOOP();
            *out_next++ = lit;
            continue;
          }
        }
      } else {

        STATIC_ASSERT(
            CAN_CONSUME_AND_THEN_PRELOAD(LITLEN_TABLEBITS, LITLEN_TABLEBITS));
        lit = entry >> 16;
        entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
        REFILL_BITS_IN_FASTLOOP();
        *out_next++ = lit;
        continue;
      }
    }

    if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {

      if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
        goto block_done;

      entry = d->u.litlen_decode_table[(entry >> 16) +
                                       EXTRACT_VARBITS(bitbuf,
                                                       (entry >> 8) & 0x3F)];
      saved_bitbuf = bitbuf;
      bitbuf >>= (u8)entry;
      bitsleft -= entry;

      if (!CAN_CONSUME_AND_THEN_PRELOAD(DEFLATE_MAX_LITLEN_CODEWORD_LEN,
                                        LITLEN_TABLEBITS) ||
          !CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXBITS, OFFSET_TABLEBITS))
        REFILL_BITS_IN_FASTLOOP();
      if (entry & HUFFDEC_LITERAL) {

        lit = entry >> 16;
        entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
        REFILL_BITS_IN_FASTLOOP();
        *out_next++ = lit;
        continue;
      }
      if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
        goto block_done;
    }

    length = entry >> 16;
    length += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);

    STATIC_ASSERT(
        CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXFASTBITS, OFFSET_TABLEBITS));
    entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
    if (CAN_CONSUME_AND_THEN_PRELOAD(OFFSET_MAXBITS, LITLEN_TABLEBITS)) {

      if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {

        if (unlikely((u8)bitsleft <
                     OFFSET_MAXBITS + LITLEN_TABLEBITS - PRELOAD_SLACK))
          REFILL_BITS_IN_FASTLOOP();
        bitbuf >>= OFFSET_TABLEBITS;
        bitsleft -= OFFSET_TABLEBITS;
        entry = d->offset_decode_table[(entry >> 16) +
                                       EXTRACT_VARBITS(bitbuf,
                                                       (entry >> 8) & 0x3F)];
      } else if (unlikely((u8)bitsleft < OFFSET_MAXFASTBITS + LITLEN_TABLEBITS -
                                             PRELOAD_SLACK))
        REFILL_BITS_IN_FASTLOOP();
    } else {

      REFILL_BITS_IN_FASTLOOP();
      if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {

        bitbuf >>= OFFSET_TABLEBITS;
        bitsleft -= OFFSET_TABLEBITS;
        entry = d->offset_decode_table[(entry >> 16) +
                                       EXTRACT_VARBITS(bitbuf,
                                                       (entry >> 8) & 0x3F)];
        REFILL_BITS_IN_FASTLOOP();

        STATIC_ASSERT(CAN_CONSUME(OFFSET_MAXBITS - OFFSET_TABLEBITS));
      } else {

        STATIC_ASSERT(CAN_CONSUME(OFFSET_MAXFASTBITS));
      }
    }
    saved_bitbuf = bitbuf;
    bitbuf >>= (u8)entry;
    bitsleft -= entry;
    offset = entry >> 16;
    offset += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);

    SAFETY_CHECK(offset <= out_next - (const u8 *)out);
    src = out_next - offset;
    dst = out_next;
    out_next += length;

    if (!CAN_CONSUME_AND_THEN_PRELOAD(
            MAX(OFFSET_MAXBITS - OFFSET_TABLEBITS, OFFSET_MAXFASTBITS),
            LITLEN_TABLEBITS) &&
        unlikely((u8)bitsleft < LITLEN_TABLEBITS - PRELOAD_SLACK))
      REFILL_BITS_IN_FASTLOOP();
    entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
    REFILL_BITS_IN_FASTLOOP();

    if (UNALIGNED_ACCESS_IS_FAST && offset >= WORDBYTES) {
      store_word_unaligned(load_word_unaligned(src), dst);
      src += WORDBYTES;
      dst += WORDBYTES;
      store_word_unaligned(load_word_unaligned(src), dst);
      src += WORDBYTES;
      dst += WORDBYTES;
      store_word_unaligned(load_word_unaligned(src), dst);
      src += WORDBYTES;
      dst += WORDBYTES;
      store_word_unaligned(load_word_unaligned(src), dst);
      src += WORDBYTES;
      dst += WORDBYTES;
      store_word_unaligned(load_word_unaligned(src), dst);
      src += WORDBYTES;
      dst += WORDBYTES;
      while (dst < out_next) {
        store_word_unaligned(load_word_unaligned(src), dst);
        src += WORDBYTES;
        dst += WORDBYTES;
        store_word_unaligned(load_word_unaligned(src), dst);
        src += WORDBYTES;
        dst += WORDBYTES;
        store_word_unaligned(load_word_unaligned(src), dst);
        src += WORDBYTES;
        dst += WORDBYTES;
        store_word_unaligned(load_word_unaligned(src), dst);
        src += WORDBYTES;
        dst += WORDBYTES;
        store_word_unaligned(load_word_unaligned(src), dst);
        src += WORDBYTES;
        dst += WORDBYTES;
      }
    } else if (UNALIGNED_ACCESS_IS_FAST && offset == 1) {
      machine_word_t v;

      v = (machine_word_t)0x0101010101010101 * src[0];
      store_word_unaligned(v, dst);
      dst += WORDBYTES;
      store_word_unaligned(v, dst);
      dst += WORDBYTES;
      store_word_unaligned(v, dst);
      dst += WORDBYTES;
      store_word_unaligned(v, dst);
      dst += WORDBYTES;
      while (dst < out_next) {
        store_word_unaligned(v, dst);
        dst += WORDBYTES;
        store_word_unaligned(v, dst);
        dst += WORDBYTES;
        store_word_unaligned(v, dst);
        dst += WORDBYTES;
        store_word_unaligned(v, dst);
        dst += WORDBYTES;
      }
    } else if (UNALIGNED_ACCESS_IS_FAST) {
      store_word_unaligned(load_word_unaligned(src), dst);
      src += offset;
      dst += offset;
      store_word_unaligned(load_word_unaligned(src), dst);
      src += offset;
      dst += offset;
      do {
        store_word_unaligned(load_word_unaligned(src), dst);
        src += offset;
        dst += offset;
        store_word_unaligned(load_word_unaligned(src), dst);
        src += offset;
        dst += offset;
      } while (dst < out_next);
    } else {
      *dst++ = *src++;
      *dst++ = *src++;
      do {
        *dst++ = *src++;
      } while (dst < out_next);
    }
  } while (in_next < in_fastloop_end && out_next < out_fastloop_end);

generic_loop:
  for (;;) {
    u32 length, offset;
    const u8 *src;
    u8 *dst;

    REFILL_BITS();
    entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
    saved_bitbuf = bitbuf;
    bitbuf >>= (u8)entry;
    bitsleft -= entry;
    if (unlikely(entry & HUFFDEC_SUBTABLE_POINTER)) {
      entry = d->u.litlen_decode_table[(entry >> 16) +
                                       EXTRACT_VARBITS(bitbuf,
                                                       (entry >> 8) & 0x3F)];
      saved_bitbuf = bitbuf;
      bitbuf >>= (u8)entry;
      bitsleft -= entry;
    }
    length = entry >> 16;
    if (entry & HUFFDEC_LITERAL) {
      if (unlikely(out_next == out_end))
        return LIBDEFLATE_INSUFFICIENT_SPACE;
      *out_next++ = length;
      continue;
    }
    if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
      goto block_done;
    length += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);
    if (unlikely(length > out_end - out_next))
      return LIBDEFLATE_INSUFFICIENT_SPACE;

    if (!CAN_CONSUME(LENGTH_MAXBITS + OFFSET_MAXBITS))
      REFILL_BITS();
    entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
    if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
      bitbuf >>= OFFSET_TABLEBITS;
      bitsleft -= OFFSET_TABLEBITS;
      entry =
          d->offset_decode_table[(entry >> 16) +
                                 EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
      if (!CAN_CONSUME(OFFSET_MAXBITS))
        REFILL_BITS();
    }
    offset = entry >> 16;
    offset += EXTRACT_VARBITS8(bitbuf, entry) >> (u8)(entry >> 8);
    bitbuf >>= (u8)entry;
    bitsleft -= entry;

    SAFETY_CHECK(offset <= out_next - (const u8 *)out);
    src = out_next - offset;
    dst = out_next;
    out_next += length;

    STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN == 3);
    *dst++ = *src++;
    *dst++ = *src++;
    do {
      *dst++ = *src++;
    } while (dst < out_next);
  }

block_done:

  if (!is_final_block)
    goto next_block;

  bitsleft = (u8)bitsleft;

  SAFETY_CHECK(overread_count <= (bitsleft >> 3));

  if (actual_in_nbytes_ret) {

    in_next -= (bitsleft >> 3) - overread_count;

    *actual_in_nbytes_ret = in_next - (u8 *)in;
  }

  if (actual_out_nbytes_ret) {
    *actual_out_nbytes_ret = out_next - (u8 *)out;
  } else {
    if (out_next != out_end)
      return LIBDEFLATE_SHORT_OUTPUT;
  }
  return LIBDEFLATE_SUCCESS;
}

#undef FUNCNAME
#undef ATTRIBUTES
#undef EXTRACT_VARBITS
#undef EXTRACT_VARBITS8
