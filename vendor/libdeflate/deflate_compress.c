/*
 * deflate_compress.c - a compressor for DEFLATE
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

#include "lib_common.h"

#include "deflate_compress.h"
#include "deflate_constants.h"

#define SUPPORT_NEAR_OPTIMAL_PARSING 1

#define MIN_BLOCK_LENGTH 5000

#define SOFT_MAX_BLOCK_LENGTH 300000

#define SEQ_STORE_LENGTH 50000

#define FAST_SOFT_MAX_BLOCK_LENGTH 65535

#define FAST_SEQ_STORE_LENGTH 8192

#define MAX_LITLEN_CODEWORD_LEN 14
#define MAX_OFFSET_CODEWORD_LEN DEFLATE_MAX_OFFSET_CODEWORD_LEN
#define MAX_PRE_CODEWORD_LEN DEFLATE_MAX_PRE_CODEWORD_LEN

#if SUPPORT_NEAR_OPTIMAL_PARSING

#define BIT_COST 16

#define LITERAL_NOSTAT_BITS 13
#define LENGTH_NOSTAT_BITS 13
#define OFFSET_NOSTAT_BITS 10

#define MATCH_CACHE_LENGTH (SOFT_MAX_BLOCK_LENGTH * 5)

#endif

#define MATCHFINDER_WINDOW_ORDER DEFLATE_WINDOW_ORDER
#include "hc_matchfinder.h"
#include "ht_matchfinder.h"
#if SUPPORT_NEAR_OPTIMAL_PARSING
#include "bt_matchfinder.h"

#define MAX_MATCHES_PER_POS (DEFLATE_MAX_MATCH_LEN - DEFLATE_MIN_MATCH_LEN + 1)
#endif

#define MAX_BLOCK_LENGTH                                                       \
  MAX(SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH - 1,                            \
      SOFT_MAX_BLOCK_LENGTH + 1 + DEFLATE_MAX_MATCH_LEN)

static forceinline void check_buildtime_parameters(void) {

  STATIC_ASSERT(SOFT_MAX_BLOCK_LENGTH >= MIN_BLOCK_LENGTH);
  STATIC_ASSERT(FAST_SOFT_MAX_BLOCK_LENGTH >= MIN_BLOCK_LENGTH);
  STATIC_ASSERT(SEQ_STORE_LENGTH * DEFLATE_MIN_MATCH_LEN >= MIN_BLOCK_LENGTH);
  STATIC_ASSERT(FAST_SEQ_STORE_LENGTH * HT_MATCHFINDER_MIN_MATCH_LEN >=
                MIN_BLOCK_LENGTH);
#if SUPPORT_NEAR_OPTIMAL_PARSING
  STATIC_ASSERT(MIN_BLOCK_LENGTH * MAX_MATCHES_PER_POS <= MATCH_CACHE_LENGTH);
#endif

  STATIC_ASSERT(FAST_SOFT_MAX_BLOCK_LENGTH <= SOFT_MAX_BLOCK_LENGTH);

  STATIC_ASSERT(SEQ_STORE_LENGTH * DEFLATE_MIN_MATCH_LEN <=
                SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH);
  STATIC_ASSERT(FAST_SEQ_STORE_LENGTH * HT_MATCHFINDER_MIN_MATCH_LEN <=
                FAST_SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH);

  STATIC_ASSERT(MAX_LITLEN_CODEWORD_LEN <= DEFLATE_MAX_LITLEN_CODEWORD_LEN);
  STATIC_ASSERT(MAX_OFFSET_CODEWORD_LEN <= DEFLATE_MAX_OFFSET_CODEWORD_LEN);
  STATIC_ASSERT(MAX_PRE_CODEWORD_LEN <= DEFLATE_MAX_PRE_CODEWORD_LEN);
  STATIC_ASSERT((1U << MAX_LITLEN_CODEWORD_LEN) >= DEFLATE_NUM_LITLEN_SYMS);
  STATIC_ASSERT((1U << MAX_OFFSET_CODEWORD_LEN) >= DEFLATE_NUM_OFFSET_SYMS);
  STATIC_ASSERT((1U << MAX_PRE_CODEWORD_LEN) >= DEFLATE_NUM_PRECODE_SYMS);
}

static const u32 deflate_length_slot_base[] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,  27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const u8 deflate_extra_length_bits[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const u32 deflate_offset_slot_base[] = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};

static const u8 deflate_extra_offset_bits[] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static const u8 deflate_length_slot[DEFLATE_MAX_MATCH_LEN + 1] = {
    0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  9,  9,  10, 10, 11, 11,
    12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16,
    16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18,
    18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28,
};

static const u8 deflate_offset_slot[256] = {
    0,  1,  2,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,
    8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15,
};

static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static const u8 deflate_extra_precode_bits[DEFLATE_NUM_PRECODE_SYMS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7};

struct deflate_codewords {
  u32 litlen[DEFLATE_NUM_LITLEN_SYMS];
  u32 offset[DEFLATE_NUM_OFFSET_SYMS];
};

struct deflate_lens {
  u8 litlen[DEFLATE_NUM_LITLEN_SYMS];
  u8 offset[DEFLATE_NUM_OFFSET_SYMS];
};

struct deflate_codes {
  struct deflate_codewords codewords;
  struct deflate_lens lens;
};

struct deflate_freqs {
  u32 litlen[DEFLATE_NUM_LITLEN_SYMS];
  u32 offset[DEFLATE_NUM_OFFSET_SYMS];
};

struct deflate_sequence {

#define SEQ_LENGTH_SHIFT 23
#define SEQ_LITRUNLEN_MASK (((u32)1 << SEQ_LENGTH_SHIFT) - 1)
  u32 litrunlen_and_length;

  u16 offset;

  u16 offset_slot;
};

#if SUPPORT_NEAR_OPTIMAL_PARSING

struct deflate_costs {

  u32 literal[DEFLATE_NUM_LITERALS];

  u32 length[DEFLATE_MAX_MATCH_LEN + 1];

  u32 offset_slot[DEFLATE_NUM_OFFSET_SYMS];
};

struct deflate_optimum_node {

  u32 cost_to_end;

#define OPTIMUM_OFFSET_SHIFT 9
#define OPTIMUM_LEN_MASK (((u32)1 << OPTIMUM_OFFSET_SHIFT) - 1)
  u32 item;
};

#endif

#define NUM_LITERAL_OBSERVATION_TYPES 8
#define NUM_MATCH_OBSERVATION_TYPES 2
#define NUM_OBSERVATION_TYPES                                                  \
  (NUM_LITERAL_OBSERVATION_TYPES + NUM_MATCH_OBSERVATION_TYPES)
#define NUM_OBSERVATIONS_PER_BLOCK_CHECK 512
struct block_split_stats {
  u32 new_observations[NUM_OBSERVATION_TYPES];
  u32 observations[NUM_OBSERVATION_TYPES];
  u32 num_new_observations;
  u32 num_observations;
};

struct deflate_output_bitstream;

struct libdeflate_compressor {

  void (*impl)(struct libdeflate_compressor *restrict c, const u8 *in,
               size_t in_nbytes, struct deflate_output_bitstream *os);

  free_func_t free_func;

  unsigned compression_level;

  size_t max_passthrough_size;

  u32 max_search_depth;

  u32 nice_match_length;

  struct deflate_freqs freqs;

  struct block_split_stats split_stats;

  struct deflate_codes codes;

  struct deflate_codes static_codes;

  union {

    struct {
      u32 freqs[DEFLATE_NUM_PRECODE_SYMS];
      u32 codewords[DEFLATE_NUM_PRECODE_SYMS];
      u8 lens[DEFLATE_NUM_PRECODE_SYMS];
      unsigned items[DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS];
      unsigned num_litlen_syms;
      unsigned num_offset_syms;
      unsigned num_explicit_lens;
      unsigned num_items;
    } precode;

    struct {
      u32 codewords[DEFLATE_MAX_MATCH_LEN + 1];
      u8 lens[DEFLATE_MAX_MATCH_LEN + 1];
    } length;
  } o;

  union {

    struct {

      struct hc_matchfinder hc_mf;

      struct deflate_sequence sequences[SEQ_STORE_LENGTH + 1];

    } g;

    struct {

      struct ht_matchfinder ht_mf;

      struct deflate_sequence sequences[FAST_SEQ_STORE_LENGTH + 1];

    } f;

#if SUPPORT_NEAR_OPTIMAL_PARSING

    struct {

      struct bt_matchfinder bt_mf;

      struct lz_match match_cache[MATCH_CACHE_LENGTH + MAX_MATCHES_PER_POS +
                                  DEFLATE_MAX_MATCH_LEN - 1];

      struct deflate_optimum_node optimum_nodes[MAX_BLOCK_LENGTH + 1];

      struct deflate_costs costs;

      struct deflate_costs costs_saved;

      u8 offset_slot_full[DEFLATE_MAX_MATCH_OFFSET + 1];

      u32 prev_observations[NUM_OBSERVATION_TYPES];
      u32 prev_num_observations;

      u32 new_match_len_freqs[DEFLATE_MAX_MATCH_LEN + 1];
      u32 match_len_freqs[DEFLATE_MAX_MATCH_LEN + 1];

      unsigned max_optim_passes;

      u32 min_improvement_to_continue;

      u32 min_bits_to_use_nonfinal_path;

      u32 max_len_to_optimize_static_block;

    } n;
#endif

  } p;
};

typedef machine_word_t bitbuf_t;

#define BITBUF_NBITS (8 * sizeof(bitbuf_t) - 1)

#define CAN_BUFFER(n) (7 + (n) <= BITBUF_NBITS)

struct deflate_output_bitstream {

  bitbuf_t bitbuf;

  unsigned bitcount;

  u8 *next;

  u8 *end;

  bool overflow;
};

#define ADD_BITS(bits, n)                                                      \
  do {                                                                         \
    bitbuf |= (bitbuf_t)(bits) << bitcount;                                    \
    bitcount += (n);                                                           \
    ASSERT(bitcount <= BITBUF_NBITS);                                          \
  } while (0)

#define FLUSH_BITS()                                                           \
  do {                                                                         \
    if (UNALIGNED_ACCESS_IS_FAST && likely(out_next < out_fast_end)) {         \
                                                                               \
      put_unaligned_leword(bitbuf, out_next);                                  \
      bitbuf >>= bitcount & ~7;                                                \
      out_next += bitcount >> 3;                                               \
      bitcount &= 7;                                                           \
    } else {                                                                   \
                                                                               \
      while (bitcount >= 8) {                                                  \
        ASSERT(out_next < os->end);                                            \
        *out_next++ = bitbuf;                                                  \
        bitcount -= 8;                                                         \
        bitbuf >>= 8;                                                          \
      }                                                                        \
    }                                                                          \
  } while (0)

static void heapify_subtree(u32 A[], unsigned length, unsigned subtree_idx) {
  unsigned parent_idx;
  unsigned child_idx;
  u32 v;

  v = A[subtree_idx];
  parent_idx = subtree_idx;
  while ((child_idx = parent_idx * 2) <= length) {
    if (child_idx < length && A[child_idx + 1] > A[child_idx])
      child_idx++;
    if (v >= A[child_idx])
      break;
    A[parent_idx] = A[child_idx];
    parent_idx = child_idx;
  }
  A[parent_idx] = v;
}

static void heapify_array(u32 A[], unsigned length) {
  unsigned subtree_idx;

  for (subtree_idx = length / 2; subtree_idx >= 1; subtree_idx--)
    heapify_subtree(A, length, subtree_idx);
}

static void heap_sort(u32 A[], unsigned length) {
  A--;

  heapify_array(A, length);

  while (length >= 2) {
    u32 tmp = A[length];

    A[length] = A[1];
    A[1] = tmp;
    length--;
    heapify_subtree(A, length, 1);
  }
}

#define NUM_SYMBOL_BITS 10
#define NUM_FREQ_BITS (32 - NUM_SYMBOL_BITS)
#define SYMBOL_MASK ((1 << NUM_SYMBOL_BITS) - 1)
#define FREQ_MASK (~SYMBOL_MASK)

#define GET_NUM_COUNTERS(num_syms) (num_syms)

static unsigned sort_symbols(unsigned num_syms, const u32 freqs[], u8 lens[],
                             u32 symout[]) {
  unsigned sym;
  unsigned i;
  unsigned num_used_syms;
  unsigned num_counters;
  unsigned counters[GET_NUM_COUNTERS(DEFLATE_MAX_NUM_SYMS)];

  num_counters = GET_NUM_COUNTERS(num_syms);

  memset(counters, 0, num_counters * sizeof(counters[0]));

  for (sym = 0; sym < num_syms; sym++)
    counters[MIN(freqs[sym], num_counters - 1)]++;

  num_used_syms = 0;
  for (i = 1; i < num_counters; i++) {
    unsigned count = counters[i];

    counters[i] = num_used_syms;
    num_used_syms += count;
  }

  for (sym = 0; sym < num_syms; sym++) {
    u32 freq = freqs[sym];

    if (freq != 0) {
      symout[counters[MIN(freq, num_counters - 1)]++] =
          sym | (freq << NUM_SYMBOL_BITS);
    } else {
      lens[sym] = 0;
    }
  }

  heap_sort(symout + counters[num_counters - 2],
            counters[num_counters - 1] - counters[num_counters - 2]);

  return num_used_syms;
}

static void build_tree(u32 A[], unsigned sym_count) {
  const unsigned last_idx = sym_count - 1;

  unsigned i = 0;

  unsigned b = 0;

  unsigned e = 0;

  do {
    u32 new_freq;

    if (i + 1 <= last_idx &&
        (b == e || (A[i + 1] & FREQ_MASK) <= (A[b] & FREQ_MASK))) {

      new_freq = (A[i] & FREQ_MASK) + (A[i + 1] & FREQ_MASK);
      i += 2;
    } else if (b + 2 <= e &&
               (i > last_idx || (A[b + 1] & FREQ_MASK) < (A[i] & FREQ_MASK))) {

      new_freq = (A[b] & FREQ_MASK) + (A[b + 1] & FREQ_MASK);
      A[b] = (e << NUM_SYMBOL_BITS) | (A[b] & SYMBOL_MASK);
      A[b + 1] = (e << NUM_SYMBOL_BITS) | (A[b + 1] & SYMBOL_MASK);
      b += 2;
    } else {

      new_freq = (A[i] & FREQ_MASK) + (A[b] & FREQ_MASK);
      A[b] = (e << NUM_SYMBOL_BITS) | (A[b] & SYMBOL_MASK);
      i++;
      b++;
    }
    A[e] = new_freq | (A[e] & SYMBOL_MASK);

  } while (++e < last_idx);
}

static void compute_length_counts(u32 A[], unsigned root_idx,
                                  unsigned len_counts[],
                                  unsigned max_codeword_len) {
  unsigned len;
  int node;

  for (len = 0; len <= max_codeword_len; len++)
    len_counts[len] = 0;
  len_counts[1] = 2;

  A[root_idx] &= SYMBOL_MASK;

  for (node = root_idx - 1; node >= 0; node--) {

    unsigned parent = A[node] >> NUM_SYMBOL_BITS;
    unsigned parent_depth = A[parent] >> NUM_SYMBOL_BITS;
    unsigned depth = parent_depth + 1;

    A[node] = (A[node] & SYMBOL_MASK) | (depth << NUM_SYMBOL_BITS);

    if (depth >= max_codeword_len) {
      depth = max_codeword_len;
      do {
        depth--;
      } while (len_counts[depth] == 0);
    }

    len_counts[depth]--;
    len_counts[depth + 1] += 2;
  }
}

#ifdef rbit32
static forceinline u32 reverse_codeword(u32 codeword, u8 len) {
  return rbit32(codeword) >> ((32 - len) & 31);
}
#else

static const u8 bitreverse_tab[256] = {
    0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0,
    0x30, 0xb0, 0x70, 0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
    0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4,
    0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
    0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc,
    0x3c, 0xbc, 0x7c, 0xfc, 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
    0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a, 0x8a, 0x4a, 0xca,
    0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
    0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6,
    0x36, 0xb6, 0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
    0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe, 0x01, 0x81, 0x41, 0xc1,
    0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
    0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9,
    0x39, 0xb9, 0x79, 0xf9, 0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
    0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5, 0x0d, 0x8d, 0x4d, 0xcd,
    0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
    0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3,
    0x33, 0xb3, 0x73, 0xf3, 0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
    0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb, 0x07, 0x87, 0x47, 0xc7,
    0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
    0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf,
    0x3f, 0xbf, 0x7f, 0xff,
};

static forceinline u32 reverse_codeword(u32 codeword, u8 len) {
  STATIC_ASSERT(DEFLATE_MAX_CODEWORD_LEN <= 16);
  codeword = ((u32)bitreverse_tab[codeword & 0xff] << 8) |
             bitreverse_tab[codeword >> 8];
  return codeword >> (16 - len);
}
#endif

static void gen_codewords(u32 A[], u8 lens[], const unsigned len_counts[],
                          unsigned max_codeword_len, unsigned num_syms) {
  u32 next_codewords[DEFLATE_MAX_CODEWORD_LEN + 1];
  unsigned i;
  unsigned len;
  unsigned sym;

  for (i = 0, len = max_codeword_len; len >= 1; len--) {
    unsigned count = len_counts[len];

    while (count--)
      lens[A[i++] & SYMBOL_MASK] = len;
  }

  next_codewords[0] = 0;
  next_codewords[1] = 0;
  for (len = 2; len <= max_codeword_len; len++)
    next_codewords[len] = (next_codewords[len - 1] + len_counts[len - 1]) << 1;

  for (sym = 0; sym < num_syms; sym++) {

    A[sym] = reverse_codeword(next_codewords[lens[sym]]++, lens[sym]);
  }
}

static void deflate_make_huffman_code(unsigned num_syms,
                                      unsigned max_codeword_len,
                                      const u32 freqs[], u8 lens[],
                                      u32 codewords[]) {
  u32 *A = codewords;
  unsigned num_used_syms;

  STATIC_ASSERT(DEFLATE_MAX_NUM_SYMS <= 1 << NUM_SYMBOL_BITS);
  STATIC_ASSERT(MAX_BLOCK_LENGTH <= ((u32)1 << NUM_FREQ_BITS) - 1);

  num_used_syms = sort_symbols(num_syms, freqs, lens, A);

  if (unlikely(num_used_syms < 2)) {
    unsigned sym = num_used_syms ? (A[0] & SYMBOL_MASK) : 0;
    unsigned nonzero_idx = sym ? sym : 1;

    codewords[0] = 0;
    lens[0] = 1;
    codewords[nonzero_idx] = 1;
    lens[nonzero_idx] = 1;
    return;
  }

  build_tree(A, num_used_syms);

  {
    unsigned len_counts[DEFLATE_MAX_CODEWORD_LEN + 1];

    compute_length_counts(A, num_used_syms - 2, len_counts, max_codeword_len);

    gen_codewords(A, lens, len_counts, max_codeword_len, num_syms);
  }
}

static void deflate_reset_symbol_frequencies(struct libdeflate_compressor *c) {
  memset(&c->freqs, 0, sizeof(c->freqs));
}

static void deflate_make_huffman_codes(const struct deflate_freqs *freqs,
                                       struct deflate_codes *codes) {
  deflate_make_huffman_code(DEFLATE_NUM_LITLEN_SYMS, MAX_LITLEN_CODEWORD_LEN,
                            freqs->litlen, codes->lens.litlen,
                            codes->codewords.litlen);

  deflate_make_huffman_code(DEFLATE_NUM_OFFSET_SYMS, MAX_OFFSET_CODEWORD_LEN,
                            freqs->offset, codes->lens.offset,
                            codes->codewords.offset);
}

static void deflate_init_static_codes(struct libdeflate_compressor *c) {
  unsigned i;

  for (i = 0; i < 144; i++)
    c->freqs.litlen[i] = 1 << (9 - 8);
  for (; i < 256; i++)
    c->freqs.litlen[i] = 1 << (9 - 9);
  for (; i < 280; i++)
    c->freqs.litlen[i] = 1 << (9 - 7);
  for (; i < 288; i++)
    c->freqs.litlen[i] = 1 << (9 - 8);

  for (i = 0; i < 32; i++)
    c->freqs.offset[i] = 1 << (5 - 5);

  deflate_make_huffman_codes(&c->freqs, &c->static_codes);
}

static forceinline unsigned deflate_get_offset_slot(u32 offset) {

  unsigned n = (256 - offset) >> 29;

  ASSERT(offset >= 1 && offset <= 32768);

  return deflate_offset_slot[(offset - 1) >> n] + (n << 1);
}

static unsigned deflate_compute_precode_items(const u8 lens[],
                                              const unsigned num_lens,
                                              u32 precode_freqs[],
                                              unsigned precode_items[]) {
  unsigned *itemptr;
  unsigned run_start;
  unsigned run_end;
  unsigned extra_bits;
  u8 len;

  memset(precode_freqs, 0, DEFLATE_NUM_PRECODE_SYMS * sizeof(precode_freqs[0]));

  itemptr = precode_items;
  run_start = 0;
  do {

    len = lens[run_start];

    run_end = run_start;
    do {
      run_end++;
    } while (run_end != num_lens && len == lens[run_end]);

    if (len == 0) {

      while ((run_end - run_start) >= 11) {
        extra_bits = MIN((run_end - run_start) - 11, 0x7F);
        precode_freqs[18]++;
        *itemptr++ = 18 | (extra_bits << 5);
        run_start += 11 + extra_bits;
      }

      if ((run_end - run_start) >= 3) {
        extra_bits = MIN((run_end - run_start) - 3, 0x7);
        precode_freqs[17]++;
        *itemptr++ = 17 | (extra_bits << 5);
        run_start += 3 + extra_bits;
      }
    } else {

      if ((run_end - run_start) >= 4) {
        precode_freqs[len]++;
        *itemptr++ = len;
        run_start++;
        do {
          extra_bits = MIN((run_end - run_start) - 3, 0x3);
          precode_freqs[16]++;
          *itemptr++ = 16 | (extra_bits << 5);
          run_start += 3 + extra_bits;
        } while ((run_end - run_start) >= 3);
      }
    }

    while (run_start != run_end) {
      precode_freqs[len]++;
      *itemptr++ = len;
      run_start++;
    }
  } while (run_start != num_lens);

  return itemptr - precode_items;
}

static void deflate_precompute_huffman_header(struct libdeflate_compressor *c) {

  for (c->o.precode.num_litlen_syms = DEFLATE_NUM_LITLEN_SYMS;
       c->o.precode.num_litlen_syms > 257; c->o.precode.num_litlen_syms--)
    if (c->codes.lens.litlen[c->o.precode.num_litlen_syms - 1] != 0)
      break;

  for (c->o.precode.num_offset_syms = DEFLATE_NUM_OFFSET_SYMS;
       c->o.precode.num_offset_syms > 1; c->o.precode.num_offset_syms--)
    if (c->codes.lens.offset[c->o.precode.num_offset_syms - 1] != 0)
      break;

  STATIC_ASSERT(offsetof(struct deflate_lens, offset) ==
                DEFLATE_NUM_LITLEN_SYMS);
  if (c->o.precode.num_litlen_syms != DEFLATE_NUM_LITLEN_SYMS) {
    memmove((u8 *)&c->codes.lens + c->o.precode.num_litlen_syms,
            (u8 *)&c->codes.lens + DEFLATE_NUM_LITLEN_SYMS,
            c->o.precode.num_offset_syms);
  }

  c->o.precode.num_items = deflate_compute_precode_items(
      (u8 *)&c->codes.lens,
      c->o.precode.num_litlen_syms + c->o.precode.num_offset_syms,
      c->o.precode.freqs, c->o.precode.items);

  deflate_make_huffman_code(DEFLATE_NUM_PRECODE_SYMS, MAX_PRE_CODEWORD_LEN,
                            c->o.precode.freqs, c->o.precode.lens,
                            c->o.precode.codewords);

  for (c->o.precode.num_explicit_lens = DEFLATE_NUM_PRECODE_SYMS;
       c->o.precode.num_explicit_lens > 4; c->o.precode.num_explicit_lens--)
    if (c->o.precode.lens[deflate_precode_lens_permutation
                              [c->o.precode.num_explicit_lens - 1]] != 0)
      break;

  if (c->o.precode.num_litlen_syms != DEFLATE_NUM_LITLEN_SYMS) {
    memmove((u8 *)&c->codes.lens + DEFLATE_NUM_LITLEN_SYMS,
            (u8 *)&c->codes.lens + c->o.precode.num_litlen_syms,
            c->o.precode.num_offset_syms);
  }
}

static void
deflate_compute_full_len_codewords(struct libdeflate_compressor *c,
                                   const struct deflate_codes *codes) {
  u32 len;

  STATIC_ASSERT(MAX_LITLEN_CODEWORD_LEN + DEFLATE_MAX_EXTRA_LENGTH_BITS <= 32);

  for (len = DEFLATE_MIN_MATCH_LEN; len <= DEFLATE_MAX_MATCH_LEN; len++) {
    unsigned slot = deflate_length_slot[len];
    unsigned litlen_sym = DEFLATE_FIRST_LEN_SYM + slot;
    u32 extra_bits = len - deflate_length_slot_base[slot];

    c->o.length.codewords[len] = codes->codewords.litlen[litlen_sym] |
                                 (extra_bits << codes->lens.litlen[litlen_sym]);
    c->o.length.lens[len] =
        codes->lens.litlen[litlen_sym] + deflate_extra_length_bits[slot];
  }
}

#define WRITE_MATCH(c_, codes_, length_, offset_, offset_slot_)                \
  do {                                                                         \
    const struct libdeflate_compressor *c__ = (c_);                            \
    const struct deflate_codes *codes__ = (codes_);                            \
    u32 length__ = (length_);                                                  \
    u32 offset__ = (offset_);                                                  \
    unsigned offset_slot__ = (offset_slot_);                                   \
                                                                               \
    STATIC_ASSERT(                                                             \
        CAN_BUFFER(MAX_LITLEN_CODEWORD_LEN + DEFLATE_MAX_EXTRA_LENGTH_BITS));  \
    ADD_BITS(c__->o.length.codewords[length__], c__->o.length.lens[length__]); \
                                                                               \
    if (!CAN_BUFFER(MAX_LITLEN_CODEWORD_LEN + DEFLATE_MAX_EXTRA_LENGTH_BITS +  \
                    MAX_OFFSET_CODEWORD_LEN + DEFLATE_MAX_EXTRA_OFFSET_BITS))  \
      FLUSH_BITS();                                                            \
                                                                               \
    ADD_BITS(codes__->codewords.offset[offset_slot__],                         \
             codes__->lens.offset[offset_slot__]);                             \
                                                                               \
    if (!CAN_BUFFER(MAX_OFFSET_CODEWORD_LEN + DEFLATE_MAX_EXTRA_OFFSET_BITS))  \
      FLUSH_BITS();                                                            \
                                                                               \
    ADD_BITS(offset__ - deflate_offset_slot_base[offset_slot__],               \
             deflate_extra_offset_bits[offset_slot__]);                        \
                                                                               \
    FLUSH_BITS();                                                              \
  } while (0)

static void deflate_flush_block(struct libdeflate_compressor *c,
                                struct deflate_output_bitstream *os,
                                const u8 *block_begin, u32 block_length,
                                const struct deflate_sequence *sequences,
                                bool is_final_block) {

  const u8 *in_next = block_begin;
  const u8 *const in_end = block_begin + block_length;
  bitbuf_t bitbuf = os->bitbuf;
  unsigned bitcount = os->bitcount;
  u8 *out_next = os->next;
  u8 *const out_fast_end = os->end - MIN(WORDBYTES - 1, os->end - out_next);

  u32 dynamic_cost = 3;
  u32 static_cost = 3;
  u32 uncompressed_cost = 3;
  u32 best_cost;
  struct deflate_codes *codes;
  unsigned sym;

  ASSERT(block_length >= MIN_BLOCK_LENGTH ||
         (is_final_block && block_length > 0));
  ASSERT(block_length <= MAX_BLOCK_LENGTH);
  ASSERT(bitcount <= 7);
  ASSERT((bitbuf & ~(((bitbuf_t)1 << bitcount) - 1)) == 0);
  ASSERT(out_next <= os->end);
  ASSERT(!os->overflow);

  deflate_precompute_huffman_header(c);

  dynamic_cost += 5 + 5 + 4 + (3 * c->o.precode.num_explicit_lens);
  for (sym = 0; sym < DEFLATE_NUM_PRECODE_SYMS; sym++) {
    u32 extra = deflate_extra_precode_bits[sym];

    dynamic_cost += c->o.precode.freqs[sym] * (extra + c->o.precode.lens[sym]);
  }

  for (sym = 0; sym < 144; sym++) {
    dynamic_cost += c->freqs.litlen[sym] * c->codes.lens.litlen[sym];
    static_cost += c->freqs.litlen[sym] * 8;
  }
  for (; sym < 256; sym++) {
    dynamic_cost += c->freqs.litlen[sym] * c->codes.lens.litlen[sym];
    static_cost += c->freqs.litlen[sym] * 9;
  }

  dynamic_cost += c->codes.lens.litlen[DEFLATE_END_OF_BLOCK];
  static_cost += 7;

  for (sym = DEFLATE_FIRST_LEN_SYM;
       sym < DEFLATE_FIRST_LEN_SYM + ARRAY_LEN(deflate_extra_length_bits);
       sym++) {
    u32 extra = deflate_extra_length_bits[sym - DEFLATE_FIRST_LEN_SYM];

    dynamic_cost += c->freqs.litlen[sym] * (extra + c->codes.lens.litlen[sym]);
    static_cost +=
        c->freqs.litlen[sym] * (extra + c->static_codes.lens.litlen[sym]);
  }

  for (sym = 0; sym < ARRAY_LEN(deflate_extra_offset_bits); sym++) {
    u32 extra = deflate_extra_offset_bits[sym];

    dynamic_cost += c->freqs.offset[sym] * (extra + c->codes.lens.offset[sym]);
    static_cost += c->freqs.offset[sym] * (extra + 5);
  }

  uncompressed_cost += (-(bitcount + 3) & 7) + 32 +
                       (40 * (DIV_ROUND_UP(block_length, UINT16_MAX) - 1)) +
                       (8 * block_length);

  best_cost = MIN(dynamic_cost, MIN(static_cost, uncompressed_cost));

  if (DIV_ROUND_UP(bitcount + best_cost, 8) > os->end - out_next) {
    os->overflow = true;
    return;
  }

  if (best_cost == uncompressed_cost) {

    do {
      u8 bfinal = 0;
      size_t len = UINT16_MAX;

      if (in_end - in_next <= UINT16_MAX) {
        bfinal = is_final_block;
        len = in_end - in_next;
      }

      ASSERT(os->end - out_next >= DIV_ROUND_UP(bitcount + 3, 8) + 4 + len);

      STATIC_ASSERT(DEFLATE_BLOCKTYPE_UNCOMPRESSED == 0);
      *out_next++ = (bfinal << bitcount) | bitbuf;
      if (bitcount > 5)
        *out_next++ = 0;
      bitbuf = 0;
      bitcount = 0;

      put_unaligned_le16(len, out_next);
      out_next += 2;
      put_unaligned_le16(~len, out_next);
      out_next += 2;
      memcpy(out_next, in_next, len);
      out_next += len;
      in_next += len;
    } while (in_next != in_end);

    goto out;
  }

  if (best_cost == static_cost) {

    codes = &c->static_codes;
    ADD_BITS(is_final_block, 1);
    ADD_BITS(DEFLATE_BLOCKTYPE_STATIC_HUFFMAN, 2);
    FLUSH_BITS();
  } else {
    const unsigned num_explicit_lens = c->o.precode.num_explicit_lens;
    const unsigned num_precode_items = c->o.precode.num_items;
    unsigned precode_sym, precode_item;
    unsigned i;

    codes = &c->codes;
    STATIC_ASSERT(CAN_BUFFER(1 + 2 + 5 + 5 + 4 + 3));
    ADD_BITS(is_final_block, 1);
    ADD_BITS(DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN, 2);
    ADD_BITS(c->o.precode.num_litlen_syms - 257, 5);
    ADD_BITS(c->o.precode.num_offset_syms - 1, 5);
    ADD_BITS(num_explicit_lens - 4, 4);

    if (CAN_BUFFER(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {

      precode_sym = deflate_precode_lens_permutation[0];
      ADD_BITS(c->o.precode.lens[precode_sym], 3);
      FLUSH_BITS();
      i = 1;
      do {
        precode_sym = deflate_precode_lens_permutation[i];
        ADD_BITS(c->o.precode.lens[precode_sym], 3);
      } while (++i < num_explicit_lens);
      FLUSH_BITS();
    } else {
      FLUSH_BITS();
      i = 0;
      do {
        precode_sym = deflate_precode_lens_permutation[i];
        ADD_BITS(c->o.precode.lens[precode_sym], 3);
        FLUSH_BITS();
      } while (++i < num_explicit_lens);
    }

    i = 0;
    do {
      precode_item = c->o.precode.items[i];
      precode_sym = precode_item & 0x1F;
      STATIC_ASSERT(CAN_BUFFER(MAX_PRE_CODEWORD_LEN + 7));
      ADD_BITS(c->o.precode.codewords[precode_sym],
               c->o.precode.lens[precode_sym]);
      ADD_BITS(precode_item >> 5, deflate_extra_precode_bits[precode_sym]);
      FLUSH_BITS();
    } while (++i < num_precode_items);
  }

  ASSERT(bitcount <= 7);
  deflate_compute_full_len_codewords(c, codes);
#if SUPPORT_NEAR_OPTIMAL_PARSING
  if (sequences == NULL) {

    struct deflate_optimum_node *cur_node = &c->p.n.optimum_nodes[0];
    struct deflate_optimum_node *const end_node =
        &c->p.n.optimum_nodes[block_length];
    do {
      u32 length = cur_node->item & OPTIMUM_LEN_MASK;
      u32 offset = cur_node->item >> OPTIMUM_OFFSET_SHIFT;

      if (length == 1) {

        ADD_BITS(codes->codewords.litlen[offset], codes->lens.litlen[offset]);
        FLUSH_BITS();
      } else {

        WRITE_MATCH(c, codes, length, offset, c->p.n.offset_slot_full[offset]);
      }
      cur_node += length;
    } while (cur_node != end_node);
  } else
#endif
  {

    const struct deflate_sequence *seq;

    for (seq = sequences;; seq++) {
      u32 litrunlen = seq->litrunlen_and_length & SEQ_LITRUNLEN_MASK;
      u32 length = seq->litrunlen_and_length >> SEQ_LENGTH_SHIFT;
      unsigned lit;

      if (CAN_BUFFER(4 * MAX_LITLEN_CODEWORD_LEN)) {
        for (; litrunlen >= 4; litrunlen -= 4) {
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          FLUSH_BITS();
        }
        if (litrunlen-- != 0) {
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          if (litrunlen-- != 0) {
            lit = *in_next++;
            ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
            if (litrunlen-- != 0) {
              lit = *in_next++;
              ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
            }
          }
          FLUSH_BITS();
        }
      } else {
        while (litrunlen--) {
          lit = *in_next++;
          ADD_BITS(codes->codewords.litlen[lit], codes->lens.litlen[lit]);
          FLUSH_BITS();
        }
      }

      if (length == 0) {
        ASSERT(in_next == in_end);
        break;
      }

      WRITE_MATCH(c, codes, length, seq->offset, seq->offset_slot);
      in_next += length;
    }
  }

  ASSERT(bitcount <= 7);
  ADD_BITS(codes->codewords.litlen[DEFLATE_END_OF_BLOCK],
           codes->lens.litlen[DEFLATE_END_OF_BLOCK]);
  FLUSH_BITS();
out:
  ASSERT(bitcount <= 7);

  ASSERT(8 * (out_next - os->next) + bitcount - os->bitcount == best_cost);
  os->bitbuf = bitbuf;
  os->bitcount = bitcount;
  os->next = out_next;
}

static void deflate_finish_block(struct libdeflate_compressor *c,
                                 struct deflate_output_bitstream *os,
                                 const u8 *block_begin, u32 block_length,
                                 const struct deflate_sequence *sequences,
                                 bool is_final_block) {
  c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;
  deflate_make_huffman_codes(&c->freqs, &c->codes);
  deflate_flush_block(c, os, block_begin, block_length, sequences,
                      is_final_block);
}

static void init_block_split_stats(struct block_split_stats *stats) {
  int i;

  for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
    stats->new_observations[i] = 0;
    stats->observations[i] = 0;
  }
  stats->num_new_observations = 0;
  stats->num_observations = 0;
}

static forceinline void observe_literal(struct block_split_stats *stats,
                                        u8 lit) {
  stats->new_observations[((lit >> 5) & 0x6) | (lit & 1)]++;
  stats->num_new_observations++;
}

static forceinline void observe_match(struct block_split_stats *stats,
                                      u32 length) {
  stats->new_observations[NUM_LITERAL_OBSERVATION_TYPES + (length >= 9)]++;
  stats->num_new_observations++;
}

static void merge_new_observations(struct block_split_stats *stats) {
  int i;

  for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
    stats->observations[i] += stats->new_observations[i];
    stats->new_observations[i] = 0;
  }
  stats->num_observations += stats->num_new_observations;
  stats->num_new_observations = 0;
}

static bool do_end_block_check(struct block_split_stats *stats,
                               u32 block_length) {
  if (stats->num_observations > 0) {

    u32 total_delta = 0;
    u32 num_items;
    u32 cutoff;
    int i;

    for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
      u32 expected = stats->observations[i] * stats->num_new_observations;
      u32 actual = stats->new_observations[i] * stats->num_observations;
      u32 delta = (actual > expected) ? actual - expected : expected - actual;

      total_delta += delta;
    }

    num_items = stats->num_observations + stats->num_new_observations;

    cutoff = stats->num_new_observations * 200 / 512 * stats->num_observations;

    if (block_length < 10000 && num_items < 8192)
      cutoff += (u64)cutoff * (8192 - num_items) / 8192;

    if (total_delta + (block_length / 4096) * stats->num_observations >= cutoff)
      return true;
  }
  merge_new_observations(stats);
  return false;
}

static forceinline bool
ready_to_check_block(const struct block_split_stats *stats,
                     const u8 *in_block_begin, const u8 *in_next,
                     const u8 *in_end) {
  return stats->num_new_observations >= NUM_OBSERVATIONS_PER_BLOCK_CHECK &&
         in_next - in_block_begin >= MIN_BLOCK_LENGTH &&
         in_end - in_next >= MIN_BLOCK_LENGTH;
}

static forceinline bool should_end_block(struct block_split_stats *stats,
                                         const u8 *in_block_begin,
                                         const u8 *in_next, const u8 *in_end) {

  if (!ready_to_check_block(stats, in_block_begin, in_next, in_end))
    return false;

  return do_end_block_check(stats, in_next - in_block_begin);
}

static void deflate_begin_sequences(struct libdeflate_compressor *c,
                                    struct deflate_sequence *first_seq) {
  deflate_reset_symbol_frequencies(c);
  first_seq->litrunlen_and_length = 0;
}

static forceinline void deflate_choose_literal(struct libdeflate_compressor *c,
                                               unsigned literal,
                                               bool gather_split_stats,
                                               struct deflate_sequence *seq) {
  c->freqs.litlen[literal]++;

  if (gather_split_stats)
    observe_literal(&c->split_stats, literal);

  STATIC_ASSERT(MAX_BLOCK_LENGTH <= SEQ_LITRUNLEN_MASK);
  seq->litrunlen_and_length++;
}

static forceinline void deflate_choose_match(struct libdeflate_compressor *c,
                                             u32 length, u32 offset,
                                             bool gather_split_stats,
                                             struct deflate_sequence **seq_p) {
  struct deflate_sequence *seq = *seq_p;
  unsigned length_slot = deflate_length_slot[length];
  unsigned offset_slot = deflate_get_offset_slot(offset);

  c->freqs.litlen[DEFLATE_FIRST_LEN_SYM + length_slot]++;
  c->freqs.offset[offset_slot]++;
  if (gather_split_stats)
    observe_match(&c->split_stats, length);

  seq->litrunlen_and_length |= length << SEQ_LENGTH_SHIFT;
  seq->offset = offset;
  seq->offset_slot = offset_slot;

  seq++;
  seq->litrunlen_and_length = 0;
  *seq_p = seq;
}

static forceinline void adjust_max_and_nice_len(u32 *max_len, u32 *nice_len,
                                                size_t remaining) {
  if (unlikely(remaining < DEFLATE_MAX_MATCH_LEN)) {
    *max_len = remaining;
    *nice_len = MIN(*nice_len, *max_len);
  }
}

static u32 choose_min_match_len(u32 num_used_literals, u32 max_search_depth) {

  static const u8 min_lens[] = {
      9, 9, 9, 9, 9, 9, 8, 8, 7, 7, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5,
      5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
      5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,

  };
  u32 min_len;

  STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN <= 3);
  STATIC_ASSERT(ARRAY_LEN(min_lens) <= DEFLATE_NUM_LITERALS + 1);

  if (num_used_literals >= ARRAY_LEN(min_lens))
    return 3;
  min_len = min_lens[num_used_literals];

  if (max_search_depth < 16) {
    if (max_search_depth < 5)
      min_len = MIN(min_len, 4);
    else if (max_search_depth < 10)
      min_len = MIN(min_len, 5);
    else
      min_len = MIN(min_len, 7);
  }
  return min_len;
}

static u32 calculate_min_match_len(const u8 *data, size_t data_len,
                                   u32 max_search_depth) {
  u8 used[256] = {0};
  u32 num_used_literals = 0;
  size_t i;

  if (data_len < 512)
    return DEFLATE_MIN_MATCH_LEN;

  data_len = MIN(data_len, 4096);
  for (i = 0; i < data_len; i++)
    used[data[i]] = 1;
  for (i = 0; i < 256; i++)
    num_used_literals += used[i];
  return choose_min_match_len(num_used_literals, max_search_depth);
}

static u32 recalculate_min_match_len(const struct deflate_freqs *freqs,
                                     u32 max_search_depth) {
  u32 literal_freq = 0;
  u32 cutoff;
  u32 num_used_literals = 0;
  int i;

  for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
    literal_freq += freqs->litlen[i];

  cutoff = literal_freq >> 10;

  for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
    if (freqs->litlen[i] > cutoff)
      num_used_literals++;
  }
  return choose_min_match_len(num_used_literals, max_search_depth);
}

static forceinline const u8 *choose_max_block_end(const u8 *in_block_begin,
                                                  const u8 *in_end,
                                                  size_t soft_max_len) {
  if (in_end - in_block_begin < soft_max_len + MIN_BLOCK_LENGTH)
    return in_end;
  return in_block_begin + soft_max_len;
}

static size_t deflate_compress_none(const u8 *in, size_t in_nbytes, u8 *out,
                                    size_t out_nbytes_avail) {
  const u8 *in_next = in;
  const u8 *const in_end = in + in_nbytes;
  u8 *out_next = out;
  u8 *const out_end = out + out_nbytes_avail;

  if (unlikely(in_nbytes == 0)) {
    if (out_nbytes_avail < 5)
      return 0;

    *out_next++ = 1 | (DEFLATE_BLOCKTYPE_UNCOMPRESSED << 1);

    put_unaligned_le32(0xFFFF0000, out_next);
    return 5;
  }

  do {
    u8 bfinal = 0;
    size_t len = UINT16_MAX;

    if (in_end - in_next <= UINT16_MAX) {
      bfinal = 1;
      len = in_end - in_next;
    }
    if (out_end - out_next < 5 + len)
      return 0;

    *out_next++ = bfinal | (DEFLATE_BLOCKTYPE_UNCOMPRESSED << 1);

    put_unaligned_le16(len, out_next);
    out_next += 2;
    put_unaligned_le16(~len, out_next);
    out_next += 2;
    memcpy(out_next, in_next, len);
    out_next += len;
    in_next += len;
  } while (in_next != in_end);

  return out_next - out;
}

static void deflate_compress_fastest(struct libdeflate_compressor *restrict c,
                                     const u8 *in, size_t in_nbytes,
                                     struct deflate_output_bitstream *os) {
  const u8 *in_next = in;
  const u8 *in_end = in_next + in_nbytes;
  const u8 *in_cur_base = in_next;
  u32 max_len = DEFLATE_MAX_MATCH_LEN;
  u32 nice_len = MIN(c->nice_match_length, max_len);
  u32 next_hash = 0;

  ht_matchfinder_init(&c->p.f.ht_mf);

  do {

    const u8 *const in_block_begin = in_next;
    const u8 *const in_max_block_end =
        choose_max_block_end(in_next, in_end, FAST_SOFT_MAX_BLOCK_LENGTH);
    struct deflate_sequence *seq = c->p.f.sequences;

    deflate_begin_sequences(c, seq);

    do {
      u32 length;
      u32 offset;
      size_t remaining = in_end - in_next;

      if (unlikely(remaining < DEFLATE_MAX_MATCH_LEN)) {
        max_len = remaining;
        if (max_len < HT_MATCHFINDER_REQUIRED_NBYTES) {
          do {
            deflate_choose_literal(c, *in_next++, false, seq);
          } while (--max_len);
          break;
        }
        nice_len = MIN(nice_len, max_len);
      }
      length =
          ht_matchfinder_longest_match(&c->p.f.ht_mf, &in_cur_base, in_next,
                                       max_len, nice_len, &next_hash, &offset);
      if (length) {

        deflate_choose_match(c, length, offset, false, &seq);
        ht_matchfinder_skip_bytes(&c->p.f.ht_mf, &in_cur_base, in_next + 1,
                                  in_end, length - 1, &next_hash);
        in_next += length;
      } else {

        deflate_choose_literal(c, *in_next++, false, seq);
      }

    } while (in_next < in_max_block_end &&
             seq < &c->p.f.sequences[FAST_SEQ_STORE_LENGTH]);

    deflate_finish_block(c, os, in_block_begin, in_next - in_block_begin,
                         c->p.f.sequences, in_next == in_end);
  } while (in_next != in_end && !os->overflow);
}

static void deflate_compress_greedy(struct libdeflate_compressor *restrict c,
                                    const u8 *in, size_t in_nbytes,
                                    struct deflate_output_bitstream *os) {
  const u8 *in_next = in;
  const u8 *in_end = in_next + in_nbytes;
  const u8 *in_cur_base = in_next;
  u32 max_len = DEFLATE_MAX_MATCH_LEN;
  u32 nice_len = MIN(c->nice_match_length, max_len);
  u32 next_hashes[2] = {0, 0};

  hc_matchfinder_init(&c->p.g.hc_mf);

  do {

    const u8 *const in_block_begin = in_next;
    const u8 *const in_max_block_end =
        choose_max_block_end(in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
    struct deflate_sequence *seq = c->p.g.sequences;
    u32 min_len;

    init_block_split_stats(&c->split_stats);
    deflate_begin_sequences(c, seq);
    min_len = calculate_min_match_len(in_next, in_max_block_end - in_next,
                                      c->max_search_depth);
    do {
      u32 length;
      u32 offset;

      adjust_max_and_nice_len(&max_len, &nice_len, in_end - in_next);
      length = hc_matchfinder_longest_match(
          &c->p.g.hc_mf, &in_cur_base, in_next, min_len - 1, max_len, nice_len,
          c->max_search_depth, next_hashes, &offset);

      if (length >= min_len &&
          (length > DEFLATE_MIN_MATCH_LEN || offset <= 4096)) {

        deflate_choose_match(c, length, offset, true, &seq);
        hc_matchfinder_skip_bytes(&c->p.g.hc_mf, &in_cur_base, in_next + 1,
                                  in_end, length - 1, next_hashes);
        in_next += length;
      } else {

        deflate_choose_literal(c, *in_next++, true, seq);
      }

    } while (
        in_next < in_max_block_end &&
        seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
        !should_end_block(&c->split_stats, in_block_begin, in_next, in_end));

    deflate_finish_block(c, os, in_block_begin, in_next - in_block_begin,
                         c->p.g.sequences, in_next == in_end);
  } while (in_next != in_end && !os->overflow);
}

static forceinline void
deflate_compress_lazy_generic(struct libdeflate_compressor *restrict c,
                              const u8 *in, size_t in_nbytes,
                              struct deflate_output_bitstream *os, bool lazy2) {
  const u8 *in_next = in;
  const u8 *in_end = in_next + in_nbytes;
  const u8 *in_cur_base = in_next;
  u32 max_len = DEFLATE_MAX_MATCH_LEN;
  u32 nice_len = MIN(c->nice_match_length, max_len);
  u32 next_hashes[2] = {0, 0};

  hc_matchfinder_init(&c->p.g.hc_mf);

  do {

    const u8 *const in_block_begin = in_next;
    const u8 *const in_max_block_end =
        choose_max_block_end(in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
    const u8 *next_recalc_min_len = in_next + MIN(in_end - in_next, 10000);
    struct deflate_sequence *seq = c->p.g.sequences;
    u32 min_len;

    init_block_split_stats(&c->split_stats);
    deflate_begin_sequences(c, seq);
    min_len = calculate_min_match_len(in_next, in_max_block_end - in_next,
                                      c->max_search_depth);
    do {
      u32 cur_len;
      u32 cur_offset;
      u32 next_len;
      u32 next_offset;

      if (in_next >= next_recalc_min_len) {
        min_len = recalculate_min_match_len(&c->freqs, c->max_search_depth);
        next_recalc_min_len +=
            MIN(in_end - next_recalc_min_len, in_next - in_block_begin);
      }

      adjust_max_and_nice_len(&max_len, &nice_len, in_end - in_next);
      cur_len = hc_matchfinder_longest_match(
          &c->p.g.hc_mf, &in_cur_base, in_next, min_len - 1, max_len, nice_len,
          c->max_search_depth, next_hashes, &cur_offset);
      if (cur_len < min_len ||
          (cur_len == DEFLATE_MIN_MATCH_LEN && cur_offset > 8192)) {

        deflate_choose_literal(c, *in_next++, true, seq);
        continue;
      }
      in_next++;

    have_cur_match:

      if (cur_len >= nice_len) {
        deflate_choose_match(c, cur_len, cur_offset, true, &seq);
        hc_matchfinder_skip_bytes(&c->p.g.hc_mf, &in_cur_base, in_next, in_end,
                                  cur_len - 1, next_hashes);
        in_next += cur_len - 1;
        continue;
      }

      adjust_max_and_nice_len(&max_len, &nice_len, in_end - in_next);
      next_len = hc_matchfinder_longest_match(
          &c->p.g.hc_mf, &in_cur_base, in_next++, cur_len - 1, max_len,
          nice_len, c->max_search_depth >> 1, next_hashes, &next_offset);
      if (next_len >= cur_len &&
          4 * (int)(next_len - cur_len) +
                  ((int)bsr32(cur_offset) - (int)bsr32(next_offset)) >
              2) {

        deflate_choose_literal(c, *(in_next - 2), true, seq);
        cur_len = next_len;
        cur_offset = next_offset;
        goto have_cur_match;
      }

      if (lazy2) {

        adjust_max_and_nice_len(&max_len, &nice_len, in_end - in_next);
        next_len = hc_matchfinder_longest_match(
            &c->p.g.hc_mf, &in_cur_base, in_next++, cur_len - 1, max_len,
            nice_len, c->max_search_depth >> 2, next_hashes, &next_offset);
        if (next_len >= cur_len &&
            4 * (int)(next_len - cur_len) +
                    ((int)bsr32(cur_offset) - (int)bsr32(next_offset)) >
                6) {

          deflate_choose_literal(c, *(in_next - 3), true, seq);
          deflate_choose_literal(c, *(in_next - 2), true, seq);
          cur_len = next_len;
          cur_offset = next_offset;
          goto have_cur_match;
        }

        deflate_choose_match(c, cur_len, cur_offset, true, &seq);
        if (cur_len > 3) {
          hc_matchfinder_skip_bytes(&c->p.g.hc_mf, &in_cur_base, in_next,
                                    in_end, cur_len - 3, next_hashes);
          in_next += cur_len - 3;
        }
      } else {

        deflate_choose_match(c, cur_len, cur_offset, true, &seq);
        hc_matchfinder_skip_bytes(&c->p.g.hc_mf, &in_cur_base, in_next, in_end,
                                  cur_len - 2, next_hashes);
        in_next += cur_len - 2;
      }

    } while (
        in_next < in_max_block_end &&
        seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
        !should_end_block(&c->split_stats, in_block_begin, in_next, in_end));

    deflate_finish_block(c, os, in_block_begin, in_next - in_block_begin,
                         c->p.g.sequences, in_next == in_end);
  } while (in_next != in_end && !os->overflow);
}

static void deflate_compress_lazy(struct libdeflate_compressor *restrict c,
                                  const u8 *in, size_t in_nbytes,
                                  struct deflate_output_bitstream *os) {
  deflate_compress_lazy_generic(c, in, in_nbytes, os, false);
}

static void deflate_compress_lazy2(struct libdeflate_compressor *restrict c,
                                   const u8 *in, size_t in_nbytes,
                                   struct deflate_output_bitstream *os) {
  deflate_compress_lazy_generic(c, in, in_nbytes, os, true);
}

#if SUPPORT_NEAR_OPTIMAL_PARSING

static void deflate_tally_item_list(struct libdeflate_compressor *c,
                                    u32 block_length) {
  struct deflate_optimum_node *cur_node = &c->p.n.optimum_nodes[0];
  struct deflate_optimum_node *end_node = &c->p.n.optimum_nodes[block_length];

  do {
    u32 length = cur_node->item & OPTIMUM_LEN_MASK;
    u32 offset = cur_node->item >> OPTIMUM_OFFSET_SHIFT;

    if (length == 1) {

      c->freqs.litlen[offset]++;
    } else {

      c->freqs.litlen[DEFLATE_FIRST_LEN_SYM + deflate_length_slot[length]]++;
      c->freqs.offset[c->p.n.offset_slot_full[offset]]++;
    }
    cur_node += length;
  } while (cur_node != end_node);

  c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;
}

static void deflate_choose_all_literals(struct libdeflate_compressor *c,
                                        const u8 *block, u32 block_length) {
  u32 i;

  deflate_reset_symbol_frequencies(c);
  for (i = 0; i < block_length; i++)
    c->freqs.litlen[block[i]]++;
  c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;

  deflate_make_huffman_codes(&c->freqs, &c->codes);
}

static u32 deflate_compute_true_cost(struct libdeflate_compressor *c) {
  u32 cost = 0;
  unsigned sym;

  deflate_precompute_huffman_header(c);

  memset(&c->codes.lens.litlen[c->o.precode.num_litlen_syms], 0,
         DEFLATE_NUM_LITLEN_SYMS - c->o.precode.num_litlen_syms);

  cost += 5 + 5 + 4 + (3 * c->o.precode.num_explicit_lens);
  for (sym = 0; sym < DEFLATE_NUM_PRECODE_SYMS; sym++) {
    cost += c->o.precode.freqs[sym] *
            (c->o.precode.lens[sym] + deflate_extra_precode_bits[sym]);
  }

  for (sym = 0; sym < DEFLATE_FIRST_LEN_SYM; sym++)
    cost += c->freqs.litlen[sym] * c->codes.lens.litlen[sym];

  for (; sym < DEFLATE_FIRST_LEN_SYM + ARRAY_LEN(deflate_extra_length_bits);
       sym++)
    cost += c->freqs.litlen[sym] *
            (c->codes.lens.litlen[sym] +
             deflate_extra_length_bits[sym - DEFLATE_FIRST_LEN_SYM]);

  for (sym = 0; sym < ARRAY_LEN(deflate_extra_offset_bits); sym++)
    cost += c->freqs.offset[sym] *
            (c->codes.lens.offset[sym] + deflate_extra_offset_bits[sym]);
  return cost;
}

static void deflate_set_costs_from_codes(struct libdeflate_compressor *c,
                                         const struct deflate_lens *lens) {
  unsigned i;

  for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
    u32 bits = (lens->litlen[i] ? lens->litlen[i] : LITERAL_NOSTAT_BITS);

    c->p.n.costs.literal[i] = bits * BIT_COST;
  }

  for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++) {
    unsigned length_slot = deflate_length_slot[i];
    unsigned litlen_sym = DEFLATE_FIRST_LEN_SYM + length_slot;
    u32 bits = (lens->litlen[litlen_sym] ? lens->litlen[litlen_sym]
                                         : LENGTH_NOSTAT_BITS);

    bits += deflate_extra_length_bits[length_slot];
    c->p.n.costs.length[i] = bits * BIT_COST;
  }

  for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++) {
    u32 bits = (lens->offset[i] ? lens->offset[i] : OFFSET_NOSTAT_BITS);

    bits += deflate_extra_offset_bits[i];
    c->p.n.costs.offset_slot[i] = bits * BIT_COST;
  }
}

static const struct {
  u8 used_lits_to_lit_cost[257];
  u8 len_sym_cost;
} default_litlen_costs[] = {
    {

        .used_lits_to_lit_cost =
            {
                6,   6,   22,  32,  38,  43,  48,  51,  54,  57,  59,  61,  64,
                65,  67,  69,  70,  72,  73,  74,  75,  76,  77,  79,  80,  80,
                81,  82,  83,  84,  85,  85,  86,  87,  88,  88,  89,  89,  90,
                91,  91,  92,  92,  93,  93,  94,  95,  95,  96,  96,  96,  97,
                97,  98,  98,  99,  99,  99,  100, 100, 101, 101, 101, 102, 102,
                102, 103, 103, 104, 104, 104, 105, 105, 105, 105, 106, 106, 106,
                107, 107, 107, 108, 108, 108, 108, 109, 109, 109, 109, 110, 110,
                110, 111, 111, 111, 111, 112, 112, 112, 112, 112, 113, 113, 113,
                113, 114, 114, 114, 114, 114, 115, 115, 115, 115, 115, 116, 116,
                116, 116, 116, 117, 117, 117, 117, 117, 118, 118, 118, 118, 118,
                118, 119, 119, 119, 119, 119, 120, 120, 120, 120, 120, 120, 121,
                121, 121, 121, 121, 121, 121, 122, 122, 122, 122, 122, 122, 123,
                123, 123, 123, 123, 123, 123, 124, 124, 124, 124, 124, 124, 124,
                125, 125, 125, 125, 125, 125, 125, 125, 126, 126, 126, 126, 126,
                126, 126, 127, 127, 127, 127, 127, 127, 127, 127, 128, 128, 128,
                128, 128, 128, 128, 128, 128, 129, 129, 129, 129, 129, 129, 129,
                129, 129, 130, 130, 130, 130, 130, 130, 130, 130, 130, 131, 131,
                131, 131, 131, 131, 131, 131, 131, 131, 132, 132, 132, 132, 132,
                132, 132, 132, 132, 132, 133, 133, 133, 133, 133, 133, 133, 133,
                133, 133, 134, 134, 134, 134, 134, 134, 134, 134,
            },
        .len_sym_cost = 109,
    },
    {

        .used_lits_to_lit_cost =
            {
                16,  16,  32,  41,  48,  53,  57,  60,  64,  66,  69,  71,  73,
                75,  76,  78,  80,  81,  82,  83,  85,  86,  87,  88,  89,  90,
                91,  92,  92,  93,  94,  95,  96,  96,  97,  98,  98,  99,  99,
                100, 101, 101, 102, 102, 103, 103, 104, 104, 105, 105, 106, 106,
                107, 107, 108, 108, 108, 109, 109, 110, 110, 110, 111, 111, 112,
                112, 112, 113, 113, 113, 114, 114, 114, 115, 115, 115, 115, 116,
                116, 116, 117, 117, 117, 118, 118, 118, 118, 119, 119, 119, 119,
                120, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 122, 122,
                123, 123, 123, 123, 124, 124, 124, 124, 124, 125, 125, 125, 125,
                125, 126, 126, 126, 126, 126, 127, 127, 127, 127, 127, 128, 128,
                128, 128, 128, 128, 129, 129, 129, 129, 129, 129, 130, 130, 130,
                130, 130, 130, 131, 131, 131, 131, 131, 131, 131, 132, 132, 132,
                132, 132, 132, 133, 133, 133, 133, 133, 133, 133, 134, 134, 134,
                134, 134, 134, 134, 134, 135, 135, 135, 135, 135, 135, 135, 135,
                136, 136, 136, 136, 136, 136, 136, 136, 137, 137, 137, 137, 137,
                137, 137, 137, 138, 138, 138, 138, 138, 138, 138, 138, 138, 139,
                139, 139, 139, 139, 139, 139, 139, 139, 140, 140, 140, 140, 140,
                140, 140, 140, 140, 141, 141, 141, 141, 141, 141, 141, 141, 141,
                141, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 142, 143,
                143, 143, 143, 143, 143, 143, 143, 143, 143, 144,
            },
        .len_sym_cost = 93,
    },
    {

        .used_lits_to_lit_cost =
            {
                32,  32,  48,  57,  64,  69,  73,  76,  80,  82,  85,  87,  89,
                91,  92,  94,  96,  97,  98,  99,  101, 102, 103, 104, 105, 106,
                107, 108, 108, 109, 110, 111, 112, 112, 113, 114, 114, 115, 115,
                116, 117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122,
                123, 123, 124, 124, 124, 125, 125, 126, 126, 126, 127, 127, 128,
                128, 128, 129, 129, 129, 130, 130, 130, 131, 131, 131, 131, 132,
                132, 132, 133, 133, 133, 134, 134, 134, 134, 135, 135, 135, 135,
                136, 136, 136, 136, 137, 137, 137, 137, 138, 138, 138, 138, 138,
                139, 139, 139, 139, 140, 140, 140, 140, 140, 141, 141, 141, 141,
                141, 142, 142, 142, 142, 142, 143, 143, 143, 143, 143, 144, 144,
                144, 144, 144, 144, 145, 145, 145, 145, 145, 145, 146, 146, 146,
                146, 146, 146, 147, 147, 147, 147, 147, 147, 147, 148, 148, 148,
                148, 148, 148, 149, 149, 149, 149, 149, 149, 149, 150, 150, 150,
                150, 150, 150, 150, 150, 151, 151, 151, 151, 151, 151, 151, 151,
                152, 152, 152, 152, 152, 152, 152, 152, 153, 153, 153, 153, 153,
                153, 153, 153, 154, 154, 154, 154, 154, 154, 154, 154, 154, 155,
                155, 155, 155, 155, 155, 155, 155, 155, 156, 156, 156, 156, 156,
                156, 156, 156, 156, 157, 157, 157, 157, 157, 157, 157, 157, 157,
                157, 158, 158, 158, 158, 158, 158, 158, 158, 158, 158, 158, 159,
                159, 159, 159, 159, 159, 159, 159, 159, 159, 160,
            },
        .len_sym_cost = 84,
    },
};

static void deflate_choose_default_litlen_costs(struct libdeflate_compressor *c,
                                                const u8 *block_begin,
                                                u32 block_length, u32 *lit_cost,
                                                u32 *len_sym_cost) {
  u32 num_used_literals = 0;
  u32 literal_freq = block_length;
  u32 match_freq = 0;
  u32 cutoff;
  u32 i;

  memset(c->freqs.litlen, 0, DEFLATE_NUM_LITERALS * sizeof(c->freqs.litlen[0]));
  cutoff = literal_freq >> 11;
  for (i = 0; i < block_length; i++)
    c->freqs.litlen[block_begin[i]]++;
  for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
    if (c->freqs.litlen[i] > cutoff)
      num_used_literals++;
  }
  if (num_used_literals == 0)
    num_used_literals = 1;

  match_freq = 0;
  i = choose_min_match_len(num_used_literals, c->max_search_depth);
  for (; i < ARRAY_LEN(c->p.n.match_len_freqs); i++) {
    match_freq += c->p.n.match_len_freqs[i];
    literal_freq -= i * c->p.n.match_len_freqs[i];
  }
  if ((s32)literal_freq < 0)
    literal_freq = 0;

  if (match_freq > literal_freq)
    i = 2;
  else if (match_freq * 4 > literal_freq)
    i = 1;
  else
    i = 0;

  STATIC_ASSERT(BIT_COST == 16);
  *lit_cost = default_litlen_costs[i].used_lits_to_lit_cost[num_used_literals];
  *len_sym_cost = default_litlen_costs[i].len_sym_cost;
}

static forceinline u32 deflate_default_length_cost(u32 len, u32 len_sym_cost) {
  unsigned slot = deflate_length_slot[len];
  u32 num_extra_bits = deflate_extra_length_bits[slot];

  return len_sym_cost + (num_extra_bits * BIT_COST);
}

static forceinline u32 deflate_default_offset_slot_cost(unsigned slot) {
  u32 num_extra_bits = deflate_extra_offset_bits[slot];

  u32 offset_sym_cost = 4 * BIT_COST + (907 * BIT_COST) / 1000;

  return offset_sym_cost + (num_extra_bits * BIT_COST);
}

static void deflate_set_default_costs(struct libdeflate_compressor *c,
                                      u32 lit_cost, u32 len_sym_cost) {
  u32 i;

  for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
    c->p.n.costs.literal[i] = lit_cost;

  for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++)
    c->p.n.costs.length[i] = deflate_default_length_cost(i, len_sym_cost);

  for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++)
    c->p.n.costs.offset_slot[i] = deflate_default_offset_slot_cost(i);
}

static forceinline void deflate_adjust_cost(u32 *cost_p, u32 default_cost,
                                            int change_amount) {
  if (change_amount == 0)

    *cost_p = (default_cost + 3 * *cost_p) / 4;
  else if (change_amount == 1)
    *cost_p = (default_cost + *cost_p) / 2;
  else if (change_amount == 2)
    *cost_p = (5 * default_cost + 3 * *cost_p) / 8;
  else

    *cost_p = (3 * default_cost + *cost_p) / 4;
}

static forceinline void
deflate_adjust_costs_impl(struct libdeflate_compressor *c, u32 lit_cost,
                          u32 len_sym_cost, int change_amount) {
  u32 i;

  for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
    deflate_adjust_cost(&c->p.n.costs.literal[i], lit_cost, change_amount);

  for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++)
    deflate_adjust_cost(&c->p.n.costs.length[i],
                        deflate_default_length_cost(i, len_sym_cost),
                        change_amount);

  for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++)
    deflate_adjust_cost(&c->p.n.costs.offset_slot[i],
                        deflate_default_offset_slot_cost(i), change_amount);
}

static void deflate_adjust_costs(struct libdeflate_compressor *c, u32 lit_cost,
                                 u32 len_sym_cost) {
  u64 total_delta = 0;
  u64 cutoff;
  int i;

  for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
    u64 prev =
        (u64)c->p.n.prev_observations[i] * c->split_stats.num_observations;
    u64 cur =
        (u64)c->split_stats.observations[i] * c->p.n.prev_num_observations;

    total_delta += prev > cur ? prev - cur : cur - prev;
  }
  cutoff = ((u64)c->p.n.prev_num_observations *
            c->split_stats.num_observations * 200) /
           512;

  if (total_delta > 3 * cutoff)

    deflate_set_default_costs(c, lit_cost, len_sym_cost);
  else if (4 * total_delta > 9 * cutoff)
    deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 3);
  else if (2 * total_delta > 3 * cutoff)
    deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 2);
  else if (2 * total_delta > cutoff)
    deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 1);
  else
    deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 0);
}

static void deflate_set_initial_costs(struct libdeflate_compressor *c,
                                      const u8 *block_begin, u32 block_length,
                                      bool is_first_block) {
  u32 lit_cost, len_sym_cost;

  deflate_choose_default_litlen_costs(c, block_begin, block_length, &lit_cost,
                                      &len_sym_cost);
  if (is_first_block)
    deflate_set_default_costs(c, lit_cost, len_sym_cost);
  else
    deflate_adjust_costs(c, lit_cost, len_sym_cost);
}

static void deflate_find_min_cost_path(struct libdeflate_compressor *c,
                                       const u32 block_length,
                                       const struct lz_match *cache_ptr) {
  struct deflate_optimum_node *end_node = &c->p.n.optimum_nodes[block_length];
  struct deflate_optimum_node *cur_node = end_node;

  cur_node->cost_to_end = 0;
  do {
    unsigned num_matches;
    u32 literal;
    u32 best_cost_to_end;

    cur_node--;
    cache_ptr--;

    num_matches = cache_ptr->length;
    literal = cache_ptr->offset;

    best_cost_to_end =
        c->p.n.costs.literal[literal] + (cur_node + 1)->cost_to_end;
    cur_node->item = (literal << OPTIMUM_OFFSET_SHIFT) | 1;

    if (num_matches) {
      const struct lz_match *match;
      u32 len;
      u32 offset;
      u32 offset_slot;
      u32 offset_cost;
      u32 cost_to_end;

      match = cache_ptr - num_matches;
      len = DEFLATE_MIN_MATCH_LEN;
      do {
        offset = match->offset;
        offset_slot = c->p.n.offset_slot_full[offset];
        offset_cost = c->p.n.costs.offset_slot[offset_slot];
        do {
          cost_to_end = offset_cost + c->p.n.costs.length[len] +
                        (cur_node + len)->cost_to_end;
          if (cost_to_end < best_cost_to_end) {
            best_cost_to_end = cost_to_end;
            cur_node->item = len | (offset << OPTIMUM_OFFSET_SHIFT);
          }
        } while (++len <= match->length);
      } while (++match != cache_ptr);
      cache_ptr -= num_matches;
    }
    cur_node->cost_to_end = best_cost_to_end;
  } while (cur_node != &c->p.n.optimum_nodes[0]);

  deflate_reset_symbol_frequencies(c);
  deflate_tally_item_list(c, block_length);
  deflate_make_huffman_codes(&c->freqs, &c->codes);
}

static void deflate_optimize_and_flush_block(
    struct libdeflate_compressor *c, struct deflate_output_bitstream *os,
    const u8 *block_begin, u32 block_length, const struct lz_match *cache_ptr,
    bool is_first_block, bool is_final_block, bool *used_only_literals) {
  unsigned num_passes_remaining = c->p.n.max_optim_passes;
  u32 best_true_cost = UINT32_MAX;
  u32 true_cost;
  u32 only_lits_cost;
  u32 static_cost = UINT32_MAX;
  struct deflate_sequence seq_;
  struct deflate_sequence *seq = NULL;
  u32 i;

  deflate_choose_all_literals(c, block_begin, block_length);
  only_lits_cost = deflate_compute_true_cost(c);

  for (i = block_length; i <= MIN(block_length - 1 + DEFLATE_MAX_MATCH_LEN,
                                  ARRAY_LEN(c->p.n.optimum_nodes) - 1);
       i++)
    c->p.n.optimum_nodes[i].cost_to_end = 0x80000000;

  if (block_length <= c->p.n.max_len_to_optimize_static_block) {

    c->p.n.costs_saved = c->p.n.costs;

    deflate_set_costs_from_codes(c, &c->static_codes.lens);
    deflate_find_min_cost_path(c, block_length, cache_ptr);
    static_cost = c->p.n.optimum_nodes[0].cost_to_end / BIT_COST;
    static_cost += 7;

    c->p.n.costs = c->p.n.costs_saved;
  }

  deflate_set_initial_costs(c, block_begin, block_length, is_first_block);

  do {

    deflate_find_min_cost_path(c, block_length, cache_ptr);

    true_cost = deflate_compute_true_cost(c);

    if (true_cost + c->p.n.min_improvement_to_continue > best_true_cost)
      break;

    best_true_cost = true_cost;

    c->p.n.costs_saved = c->p.n.costs;

    deflate_set_costs_from_codes(c, &c->codes.lens);

  } while (--num_passes_remaining);

  *used_only_literals = false;
  if (MIN(only_lits_cost, static_cost) < best_true_cost) {
    if (only_lits_cost < static_cost) {

      deflate_choose_all_literals(c, block_begin, block_length);
      deflate_set_costs_from_codes(c, &c->codes.lens);
      seq_.litrunlen_and_length = block_length;
      seq = &seq_;
      *used_only_literals = true;
    } else {

      deflate_set_costs_from_codes(c, &c->static_codes.lens);
      deflate_find_min_cost_path(c, block_length, cache_ptr);
    }
  } else if (true_cost >=
             best_true_cost + c->p.n.min_bits_to_use_nonfinal_path) {

    c->p.n.costs = c->p.n.costs_saved;
    deflate_find_min_cost_path(c, block_length, cache_ptr);
    deflate_set_costs_from_codes(c, &c->codes.lens);
  }
  deflate_flush_block(c, os, block_begin, block_length, seq, is_final_block);
}

static void deflate_near_optimal_init_stats(struct libdeflate_compressor *c) {
  init_block_split_stats(&c->split_stats);
  memset(c->p.n.new_match_len_freqs, 0, sizeof(c->p.n.new_match_len_freqs));
  memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

static void deflate_near_optimal_merge_stats(struct libdeflate_compressor *c) {
  unsigned i;

  merge_new_observations(&c->split_stats);
  for (i = 0; i < ARRAY_LEN(c->p.n.match_len_freqs); i++) {
    c->p.n.match_len_freqs[i] += c->p.n.new_match_len_freqs[i];
    c->p.n.new_match_len_freqs[i] = 0;
  }
}

static void deflate_near_optimal_save_stats(struct libdeflate_compressor *c) {
  int i;

  for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
    c->p.n.prev_observations[i] = c->split_stats.observations[i];
  c->p.n.prev_num_observations = c->split_stats.num_observations;
}

static void
deflate_near_optimal_clear_old_stats(struct libdeflate_compressor *c) {
  int i;

  for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
    c->split_stats.observations[i] = 0;
  c->split_stats.num_observations = 0;
  memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

static void
deflate_compress_near_optimal(struct libdeflate_compressor *restrict c,
                              const u8 *in, size_t in_nbytes,
                              struct deflate_output_bitstream *os) {
  const u8 *in_next = in;
  const u8 *in_block_begin = in_next;
  const u8 *in_end = in_next + in_nbytes;
  const u8 *in_cur_base = in_next;
  const u8 *in_next_slide =
      in_next + MIN(in_end - in_next, MATCHFINDER_WINDOW_SIZE);
  u32 max_len = DEFLATE_MAX_MATCH_LEN;
  u32 nice_len = MIN(c->nice_match_length, max_len);
  struct lz_match *cache_ptr = c->p.n.match_cache;
  u32 next_hashes[2] = {0, 0};
  bool prev_block_used_only_literals = false;

  bt_matchfinder_init(&c->p.n.bt_mf);
  deflate_near_optimal_init_stats(c);

  do {

    const u8 *const in_max_block_end =
        choose_max_block_end(in_block_begin, in_end, SOFT_MAX_BLOCK_LENGTH);
    const u8 *prev_end_block_check = NULL;
    bool change_detected = false;
    const u8 *next_observation = in_next;
    u32 min_len;

    if (prev_block_used_only_literals)
      min_len = DEFLATE_MAX_MATCH_LEN + 1;
    else
      min_len = calculate_min_match_len(in_block_begin,
                                        in_max_block_end - in_block_begin,
                                        c->max_search_depth);

    for (;;) {
      struct lz_match *matches;
      u32 best_len;
      size_t remaining = in_end - in_next;

      if (in_next == in_next_slide) {
        bt_matchfinder_slide_window(&c->p.n.bt_mf);
        in_cur_base = in_next;
        in_next_slide = in_next + MIN(remaining, MATCHFINDER_WINDOW_SIZE);
      }

      matches = cache_ptr;
      best_len = 0;
      adjust_max_and_nice_len(&max_len, &nice_len, remaining);
      if (likely(max_len >= BT_MATCHFINDER_REQUIRED_NBYTES)) {
        cache_ptr = bt_matchfinder_get_matches(
            &c->p.n.bt_mf, in_cur_base, in_next - in_cur_base, max_len,
            nice_len, c->max_search_depth, next_hashes, matches);
        if (cache_ptr > matches)
          best_len = cache_ptr[-1].length;
      }
      if (in_next >= next_observation) {
        if (best_len >= min_len) {
          observe_match(&c->split_stats, best_len);
          next_observation = in_next + best_len;
          c->p.n.new_match_len_freqs[best_len]++;
        } else {
          observe_literal(&c->split_stats, *in_next);
          next_observation = in_next + 1;
        }
      }

      cache_ptr->length = cache_ptr - matches;
      cache_ptr->offset = *in_next;
      in_next++;
      cache_ptr++;

      if (best_len >= DEFLATE_MIN_MATCH_LEN && best_len >= nice_len) {
        --best_len;
        do {
          remaining = in_end - in_next;
          if (in_next == in_next_slide) {
            bt_matchfinder_slide_window(&c->p.n.bt_mf);
            in_cur_base = in_next;
            in_next_slide = in_next + MIN(remaining, MATCHFINDER_WINDOW_SIZE);
          }
          adjust_max_and_nice_len(&max_len, &nice_len, remaining);
          if (max_len >= BT_MATCHFINDER_REQUIRED_NBYTES) {
            bt_matchfinder_skip_byte(&c->p.n.bt_mf, in_cur_base,
                                     in_next - in_cur_base, nice_len,
                                     c->max_search_depth, next_hashes);
          }
          cache_ptr->length = 0;
          cache_ptr->offset = *in_next;
          in_next++;
          cache_ptr++;
        } while (--best_len);
      }

      if (in_next >= in_max_block_end)
        break;

      if (cache_ptr >= &c->p.n.match_cache[MATCH_CACHE_LENGTH])
        break;

      if (!ready_to_check_block(&c->split_stats, in_block_begin, in_next,
                                in_end))
        continue;

      if (do_end_block_check(&c->split_stats, in_next - in_block_begin)) {
        change_detected = true;
        break;
      }

      deflate_near_optimal_merge_stats(c);
      prev_end_block_check = in_next;
    }

    if (change_detected && prev_end_block_check != NULL) {

      struct lz_match *orig_cache_ptr = cache_ptr;
      const u8 *in_block_end = prev_end_block_check;
      u32 block_length = in_block_end - in_block_begin;
      bool is_first = (in_block_begin == in);
      bool is_final = false;
      u32 num_bytes_to_rewind = in_next - in_block_end;
      size_t cache_len_rewound;

      do {
        cache_ptr--;
        cache_ptr -= cache_ptr->length;
      } while (--num_bytes_to_rewind);
      cache_len_rewound = orig_cache_ptr - cache_ptr;

      deflate_optimize_and_flush_block(c, os, in_block_begin, block_length,
                                       cache_ptr, is_first, is_final,
                                       &prev_block_used_only_literals);
      memmove(c->p.n.match_cache, cache_ptr,
              cache_len_rewound * sizeof(*cache_ptr));
      cache_ptr = &c->p.n.match_cache[cache_len_rewound];
      deflate_near_optimal_save_stats(c);

      deflate_near_optimal_clear_old_stats(c);
      in_block_begin = in_block_end;
    } else {

      u32 block_length = in_next - in_block_begin;
      bool is_first = (in_block_begin == in);
      bool is_final = (in_next == in_end);

      deflate_near_optimal_merge_stats(c);
      deflate_optimize_and_flush_block(c, os, in_block_begin, block_length,
                                       cache_ptr, is_first, is_final,
                                       &prev_block_used_only_literals);
      cache_ptr = &c->p.n.match_cache[0];
      deflate_near_optimal_save_stats(c);
      deflate_near_optimal_init_stats(c);
      in_block_begin = in_next;
    }
  } while (in_next != in_end && !os->overflow);
}

static void deflate_init_offset_slot_full(struct libdeflate_compressor *c) {
  u32 offset_slot;
  u32 offset;
  u32 offset_end;

  for (offset_slot = 0; offset_slot < ARRAY_LEN(deflate_offset_slot_base);
       offset_slot++) {
    offset = deflate_offset_slot_base[offset_slot];
    offset_end = offset + (1 << deflate_extra_offset_bits[offset_slot]);
    do {
      c->p.n.offset_slot_full[offset] = offset_slot;
    } while (++offset != offset_end);
  }
}

#endif

LIBDEFLATEAPI struct libdeflate_compressor *
libdeflate_alloc_compressor_ex(int compression_level,
                               const struct libdeflate_options *options) {
  struct libdeflate_compressor *c;
  size_t size = offsetof(struct libdeflate_compressor, p);

  check_buildtime_parameters();

  if (options->sizeof_options != sizeof(*options))
    return NULL;

  if (compression_level == -1)
    compression_level = 6;

  if (compression_level < 0 || compression_level > 12)
    return NULL;

#if SUPPORT_NEAR_OPTIMAL_PARSING
  if (compression_level >= 10)
    size += sizeof(c->p.n);
  else
#endif
  {
    if (compression_level >= 2)
      size += sizeof(c->p.g);
    else if (compression_level == 1)
      size += sizeof(c->p.f);
  }

  c = libdeflate_aligned_malloc(options->malloc_func
                                    ? options->malloc_func
                                    : libdeflate_default_malloc_func,
                                MATCHFINDER_MEM_ALIGNMENT, size);
  if (!c)
    return NULL;
  c->free_func =
      options->free_func ? options->free_func : libdeflate_default_free_func;

  c->compression_level = compression_level;

  c->max_passthrough_size = 55 - (compression_level * 4);

  switch (compression_level) {
  case 0:
    c->max_passthrough_size = SIZE_MAX;
    c->impl = NULL;
    break;
  case 1:
    c->impl = deflate_compress_fastest;

    c->nice_match_length = 32;
    break;
  case 2:
    c->impl = deflate_compress_greedy;
    c->max_search_depth = 6;
    c->nice_match_length = 10;
    break;
  case 3:
    c->impl = deflate_compress_greedy;
    c->max_search_depth = 12;
    c->nice_match_length = 14;
    break;
  case 4:
    c->impl = deflate_compress_greedy;
    c->max_search_depth = 16;
    c->nice_match_length = 30;
    break;
  case 5:
    c->impl = deflate_compress_lazy;
    c->max_search_depth = 16;
    c->nice_match_length = 30;
    break;
  case 6:
    c->impl = deflate_compress_lazy;
    c->max_search_depth = 35;
    c->nice_match_length = 65;
    break;
  case 7:
    c->impl = deflate_compress_lazy;
    c->max_search_depth = 100;
    c->nice_match_length = 130;
    break;
  case 8:
    c->impl = deflate_compress_lazy2;
    c->max_search_depth = 300;
    c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
    break;
  case 9:
#if !SUPPORT_NEAR_OPTIMAL_PARSING
  default:
#endif
    c->impl = deflate_compress_lazy2;
    c->max_search_depth = 600;
    c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
    break;
#if SUPPORT_NEAR_OPTIMAL_PARSING
  case 10:
    c->impl = deflate_compress_near_optimal;
    c->max_search_depth = 35;
    c->nice_match_length = 75;
    c->p.n.max_optim_passes = 2;
    c->p.n.min_improvement_to_continue = 32;
    c->p.n.min_bits_to_use_nonfinal_path = 32;
    c->p.n.max_len_to_optimize_static_block = 0;
    deflate_init_offset_slot_full(c);
    break;
  case 11:
    c->impl = deflate_compress_near_optimal;
    c->max_search_depth = 100;
    c->nice_match_length = 150;
    c->p.n.max_optim_passes = 4;
    c->p.n.min_improvement_to_continue = 16;
    c->p.n.min_bits_to_use_nonfinal_path = 16;
    c->p.n.max_len_to_optimize_static_block = 1000;
    deflate_init_offset_slot_full(c);
    break;
  case 12:
  default:
    c->impl = deflate_compress_near_optimal;
    c->max_search_depth = 300;
    c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
    c->p.n.max_optim_passes = 10;
    c->p.n.min_improvement_to_continue = 1;
    c->p.n.min_bits_to_use_nonfinal_path = 1;
    c->p.n.max_len_to_optimize_static_block = 10000;
    deflate_init_offset_slot_full(c);
    break;
#endif
  }

  deflate_init_static_codes(c);

  return c;
}

LIBDEFLATEAPI struct libdeflate_compressor *
libdeflate_alloc_compressor(int compression_level) {
  static const struct libdeflate_options defaults = {
      .sizeof_options = sizeof(defaults),
  };
  return libdeflate_alloc_compressor_ex(compression_level, &defaults);
}

LIBDEFLATEAPI size_t libdeflate_deflate_compress(
    struct libdeflate_compressor *c, const void *in, size_t in_nbytes,
    void *out, size_t out_nbytes_avail) {
  struct deflate_output_bitstream os;

  if (unlikely(in_nbytes <= c->max_passthrough_size))
    return deflate_compress_none(in, in_nbytes, out, out_nbytes_avail);

  os.bitbuf = 0;
  os.bitcount = 0;
  os.next = out;
  os.end = os.next + out_nbytes_avail;
  os.overflow = false;

  (*c->impl)(c, in, in_nbytes, &os);

  if (os.overflow)
    return 0;

  ASSERT(os.bitcount <= 7);
  if (os.bitcount) {
    ASSERT(os.next < os.end);
    *os.next++ = os.bitbuf;
  }

  return os.next - (u8 *)out;
}

LIBDEFLATEAPI void libdeflate_free_compressor(struct libdeflate_compressor *c) {
  if (c)
    libdeflate_aligned_free(c->free_func, c);
}

unsigned int libdeflate_get_compression_level(struct libdeflate_compressor *c) {
  return c->compression_level;
}

LIBDEFLATEAPI size_t libdeflate_deflate_compress_bound(
    struct libdeflate_compressor *c, size_t in_nbytes) {
  size_t max_blocks;

  STATIC_ASSERT(2 * MIN_BLOCK_LENGTH <= UINT16_MAX);
  max_blocks = MAX(DIV_ROUND_UP(in_nbytes, MIN_BLOCK_LENGTH), 1);

  return (5 * max_blocks) + in_nbytes;
}
