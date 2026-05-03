/* deflate.h -- internal compression state
 * Copyright (C) 1995-2018 Jean-loup Gailly
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef DEFLATE_H
#define DEFLATE_H

#include "zutil.h"

#define LENGTH_CODES 29

#define LITERALS 256

#define L_CODES (LITERALS + 1 + LENGTH_CODES)

#define D_CODES 30

#define BL_CODES 19

#define HEAP_SIZE (2 * L_CODES + 1)

#define MAX_BITS 15

#define Buf_size 64

#define INIT_STATE 42
#define EXTRA_STATE 69
#define NAME_STATE 73
#define COMMENT_STATE 91
#define HCRC_STATE 103
#define BUSY_STATE 113
#define FINISH_STATE 666

typedef struct ct_data_s {
  union {
    uint16_t freq;
    uint16_t code;
  } fc;
  union {
    uint16_t dad;
    uint16_t len;
  } dl;
} ct_data;

#define Freq fc.freq
#define Code fc.code
#define Dad dl.dad
#define Len dl.len

typedef struct static_tree_desc_s static_tree_desc;

typedef struct tree_desc_s {
  ct_data *dyn_tree;
  int max_code;
  static_tree_desc *stat_desc;
} tree_desc;

typedef uint16_t Pos;
typedef uint32_t IPos;

typedef struct internal_state {
  z_streamp strm;
  int status;
  uint8_t *pending_buf;
  uint64_t pending_buf_size;
  uint8_t *pending_out;
  uint32_t pending;
  int wrap;
  gz_headerp gzhead;
  uint32_t gzindex;
  uint8_t method;
  int last_flush;

  uint32_t w_size;
  uint32_t w_bits;
  uint32_t w_mask;

  uint8_t *window;

  uint32_t window_size;

  Pos *prev;

  Pos *head;

  uint32_t ins_h;
  uint32_t hash_size;
  uint32_t hash_bits;
  uint32_t hash_mask;

  uint32_t hash_shift;

  long block_start;

  uint32_t match_length;
  IPos prev_match;
  int match_available;
  uint32_t strstart;
  uint32_t match_start;
  uint32_t lookahead;

  uint32_t prev_length;

  uint32_t max_chain_length;

  uint32_t max_lazy_match;

#define max_insert_length max_lazy_match

  int level;
  int strategy;

  uint32_t good_match;

  int nice_match;

  struct ct_data_s dyn_ltree[HEAP_SIZE];
  struct ct_data_s dyn_dtree[2 * D_CODES + 1];
  struct ct_data_s bl_tree[2 * BL_CODES + 1];

  struct tree_desc_s l_desc;
  struct tree_desc_s d_desc;
  struct tree_desc_s bl_desc;

  uint16_t bl_count[MAX_BITS + 1];

  int heap[2 * L_CODES + 1];
  int heap_len;
  int heap_max;

  uint8_t depth[2 * L_CODES + 1];

  uint8_t *sym_buf;

  uint32_t lit_bufsize;

  uInt sym_next;
  uInt sym_end;

  uint64_t opt_len;
  uint64_t static_len;
  uint32_t matches;
  uint32_t insert;

#ifdef ZLIB_DEBUG
  uint64_t compressed_len;
  uint64_t bits_sent;
#endif

  uint64_t bi_buf;

  int bi_valid;

  uint64_t high_water;

} deflate_state;

#define put_byte(s, c)                                                         \
  {                                                                            \
    s->pending_buf[s->pending++] = (c);                                        \
  }

#define put_short(s, w)                                                        \
  {                                                                            \
    s->pending += 2;                                                           \
    *(ush *)(&s->pending_buf[s->pending - 2]) = (w);                           \
  }

#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)

#define MAX_DIST(s) ((s)->w_size - MIN_LOOKAHEAD)

#define WIN_INIT MAX_MATCH

void ZLIB_INTERNAL _tr_init(deflate_state *s);
int ZLIB_INTERNAL _tr_tally(deflate_state *s, uint32_t dist, unsigned lc);
void ZLIB_INTERNAL _tr_flush_block(deflate_state *s, uint8_t *buf,
                                   uint64_t stored_len, int last);
void ZLIB_INTERNAL _tr_flush_bits(deflate_state *s);
void ZLIB_INTERNAL _tr_align(deflate_state *s);
void ZLIB_INTERNAL _tr_stored_block(deflate_state *s, uint8_t *buf,
                                    uint64_t stored_len, int last);

#define d_code(dist)                                                           \
  ((dist) < 256 ? _dist_code[dist] : _dist_code[256 + ((dist) >> 7)])

extern const uint8_t ZLIB_INTERNAL _length_code[];
extern const uint8_t ZLIB_INTERNAL _dist_code[];

#define _tr_tally_lit(s, c, flush)                                             \
  {                                                                            \
    uch cc = (c);                                                              \
    s->sym_buf[s->sym_next++] = 0;                                             \
    s->sym_buf[s->sym_next++] = 0;                                             \
    s->sym_buf[s->sym_next++] = cc;                                            \
    s->dyn_ltree[cc].Freq++;                                                   \
    flush = (s->sym_next == s->sym_end);                                       \
  }
#define _tr_tally_dist(s, distance, length, flush)                             \
  {                                                                            \
    uch len = (uch)(length);                                                   \
    ush dist = (ush)(distance);                                                \
    s->sym_buf[s->sym_next++] = dist;                                          \
    s->sym_buf[s->sym_next++] = dist >> 8;                                     \
    s->sym_buf[s->sym_next++] = len;                                           \
    dist--;                                                                    \
    s->dyn_ltree[_length_code[len] + LITERALS + 1].Freq++;                     \
    s->dyn_dtree[d_code(dist)].Freq++;                                         \
    flush = (s->sym_next == s->sym_end);                                       \
  }

#if defined(_MSC_VER) && !defined(__clang__)

#define likely(x) (x)
#define unlikely(x) (x)

int __inline __builtin_ctzl(unsigned long mask) {
  unsigned long index;

  return _BitScanForward(&index, mask) == 0 ? 32 : ((int)index);
}
#else
#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)
#endif

#endif
