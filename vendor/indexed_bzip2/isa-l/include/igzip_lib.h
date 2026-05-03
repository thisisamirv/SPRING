/**********************************************************************
  Copyright(c) 2011-2016 Intel Corporation All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************/

#ifndef _IGZIP_H
#define _IGZIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IGZIP_K 1024
#define ISAL_DEF_MAX_HDR_SIZE 328
#define ISAL_DEF_MAX_CODE_LEN 15
#define ISAL_DEF_HIST_SIZE (32 * IGZIP_K)
#define ISAL_DEF_MAX_HIST_BITS 15
#define ISAL_DEF_MAX_MATCH 258
#define ISAL_DEF_MIN_MATCH 3

#define ISAL_DEF_LIT_SYMBOLS 257
#define ISAL_DEF_LEN_SYMBOLS 29
#define ISAL_DEF_DIST_SYMBOLS 30
#define ISAL_DEF_LIT_LEN_SYMBOLS (ISAL_DEF_LIT_SYMBOLS + ISAL_DEF_LEN_SYMBOLS)

#define ISAL_LOOK_AHEAD ((ISAL_DEF_MAX_MATCH + 31) & ~31)

#ifndef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE ISAL_DEF_HIST_SIZE
#endif

#if (IGZIP_HIST_SIZE > ISAL_DEF_HIST_SIZE)
#undef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE ISAL_DEF_HIST_SIZE
#endif

#ifdef LONGER_HUFFTABLE
#if (IGZIP_HIST_SIZE > 8 * IGZIP_K)
#undef IGZIP_HIST_SIZE
#define IGZIP_HIST_SIZE (8 * IGZIP_K)
#endif
#endif

#define ISAL_LIMIT_HASH_UPDATE

#define IGZIP_HASH8K_HASH_SIZE (8 * IGZIP_K)
#define IGZIP_HASH_HIST_SIZE IGZIP_HIST_SIZE
#define IGZIP_HASH_MAP_HASH_SIZE IGZIP_HIST_SIZE

#define IGZIP_LVL0_HASH_SIZE (8 * IGZIP_K)
#define IGZIP_LVL1_HASH_SIZE IGZIP_HASH8K_HASH_SIZE
#define IGZIP_LVL2_HASH_SIZE IGZIP_HASH_HIST_SIZE
#define IGZIP_LVL3_HASH_SIZE IGZIP_HASH_MAP_HASH_SIZE

#ifdef LONGER_HUFFTABLE
enum { IGZIP_DIST_TABLE_SIZE = 8 * 1024 };

enum { IGZIP_DECODE_OFFSET = 26 };
#else
enum { IGZIP_DIST_TABLE_SIZE = 2 };

enum { IGZIP_DECODE_OFFSET = 0 };
#endif
enum { IGZIP_LEN_TABLE_SIZE = 256 };
enum { IGZIP_LIT_TABLE_SIZE = ISAL_DEF_LIT_SYMBOLS };

#define IGZIP_HUFFTABLE_CUSTOM 0
#define IGZIP_HUFFTABLE_DEFAULT 1
#define IGZIP_HUFFTABLE_STATIC 2

#define NO_FLUSH 0
#define SYNC_FLUSH 1
#define FULL_FLUSH 2
#define FINISH_FLUSH 0

#define IGZIP_DEFLATE 0
#define IGZIP_GZIP 1
#define IGZIP_GZIP_NO_HDR 2
#define IGZIP_ZLIB 3
#define IGZIP_ZLIB_NO_HDR 4

#define COMP_OK 0
#define INVALID_FLUSH -7
#define INVALID_PARAM -8
#define STATELESS_OVERFLOW -1
#define ISAL_INVALID_OPERATION -9
#define ISAL_INVALID_STATE -3
#define ISAL_INVALID_LEVEL -4
#define ISAL_INVALID_LEVEL_BUF -5

enum isal_zstate_state {
  ZSTATE_NEW_HDR,
  ZSTATE_HDR,
  ZSTATE_CREATE_HDR,
  ZSTATE_BODY,
  ZSTATE_FLUSH_READ_BUFFER,
  ZSTATE_FLUSH_ICF_BUFFER,
  ZSTATE_TYPE0_HDR,
  ZSTATE_TYPE0_BODY,
  ZSTATE_SYNC_FLUSH,
  ZSTATE_FLUSH_WRITE_BUFFER,
  ZSTATE_TRL,
  ZSTATE_END,
  ZSTATE_TMP_NEW_HDR,
  ZSTATE_TMP_HDR,
  ZSTATE_TMP_CREATE_HDR,
  ZSTATE_TMP_BODY,
  ZSTATE_TMP_FLUSH_READ_BUFFER,
  ZSTATE_TMP_FLUSH_ICF_BUFFER,
  ZSTATE_TMP_TYPE0_HDR,
  ZSTATE_TMP_TYPE0_BODY,
  ZSTATE_TMP_SYNC_FLUSH,
  ZSTATE_TMP_FLUSH_WRITE_BUFFER,
  ZSTATE_TMP_TRL,
  ZSTATE_TMP_END
};

#define ZSTATE_TMP_OFFSET ZSTATE_TMP_HDR - ZSTATE_HDR

#define ISAL_DECODE_LONG_BITS 12
#define ISAL_DECODE_SHORT_BITS 10

enum isal_block_state {
  ISAL_BLOCK_NEW_HDR,
  ISAL_BLOCK_HDR,
  ISAL_BLOCK_TYPE0,
  ISAL_BLOCK_CODED,
  ISAL_BLOCK_INPUT_DONE,
  ISAL_BLOCK_FINISH,

  ISAL_GZIP_EXTRA_LEN,
  ISAL_GZIP_EXTRA,
  ISAL_GZIP_NAME,
  ISAL_GZIP_COMMENT,
  ISAL_GZIP_HCRC,
  ISAL_ZLIB_DICT,
  ISAL_CHECKSUM_CHECK,
};

enum isal_stopping_point {
  ISAL_STOPPING_POINT_NONE = 0,
  ISAL_STOPPING_POINT_END_OF_STREAM_HEADER = 1U << 0U,
  ISAL_STOPPING_POINT_END_OF_STREAM = 1U << 1U,
  ISAL_STOPPING_POINT_END_OF_BLOCK_HEADER = 1U << 2U,
  ISAL_STOPPING_POINT_END_OF_BLOCK = 1U << 3U,
  ISAL_STOPPING_POINT_ALL = 0xFFFFFFFFU,
};

#define ISAL_DEFLATE 0
#define ISAL_GZIP 1
#define ISAL_GZIP_NO_HDR 2
#define ISAL_ZLIB 3
#define ISAL_ZLIB_NO_HDR 4
#define ISAL_ZLIB_NO_HDR_VER 5
#define ISAL_GZIP_NO_HDR_VER 6

#define ISAL_DECOMP_OK 0
#define ISAL_END_INPUT 1
#define ISAL_OUT_OVERFLOW 2
#define ISAL_NAME_OVERFLOW 3
#define ISAL_COMMENT_OVERFLOW 4
#define ISAL_EXTRA_OVERFLOW 5
#define ISAL_NEED_DICT 6
#define ISAL_INVALID_BLOCK -1
#define ISAL_INVALID_SYMBOL -2
#define ISAL_INVALID_LOOKBACK -3
#define ISAL_INVALID_WRAPPER -4
#define ISAL_UNSUPPORTED_METHOD -5
#define ISAL_INCORRECT_CHECKSUM -6

struct isal_huff_histogram {
  uint64_t lit_len_histogram[ISAL_DEF_LIT_LEN_SYMBOLS];

  uint64_t dist_histogram[ISAL_DEF_DIST_SYMBOLS];

  uint16_t hash_table[IGZIP_LVL0_HASH_SIZE];
};

struct isal_mod_hist {
  uint32_t d_hist[30];
  uint32_t ll_hist[513];
};

#define ISAL_DEF_MIN_LEVEL 0
#define ISAL_DEF_MAX_LEVEL 3

#define ISAL_DEF_LVL0_REQ 0
#define ISAL_DEF_LVL1_REQ (4 * IGZIP_K + 2 * IGZIP_LVL1_HASH_SIZE)
#define ISAL_DEF_LVL1_TOKEN_SIZE 4
#define ISAL_DEF_LVL2_REQ (4 * IGZIP_K + 2 * IGZIP_LVL2_HASH_SIZE)
#define ISAL_DEF_LVL2_TOKEN_SIZE 4
#define ISAL_DEF_LVL3_REQ                                                      \
  4 * IGZIP_K + 4 * 4 * IGZIP_K + 2 * IGZIP_LVL3_HASH_SIZE
#define ISAL_DEF_LVL3_TOKEN_SIZE 4

#define ISAL_DEF_LVL0_MIN ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_SMALL ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_MEDIUM ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_LARGE ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_EXTRA_LARGE ISAL_DEF_LVL0_REQ
#define ISAL_DEF_LVL0_DEFAULT ISAL_DEF_LVL0_REQ

#define ISAL_DEF_LVL1_MIN                                                      \
  (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL1_SMALL                                                    \
  (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL1_MEDIUM                                                   \
  (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL1_LARGE                                                    \
  (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL1_EXTRA_LARGE                                              \
  (ISAL_DEF_LVL1_REQ + ISAL_DEF_LVL1_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL1_DEFAULT ISAL_DEF_LVL1_LARGE

#define ISAL_DEF_LVL2_MIN                                                      \
  (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL2_SMALL                                                    \
  (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL2_MEDIUM                                                   \
  (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL2_LARGE                                                    \
  (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL2_EXTRA_LARGE                                              \
  (ISAL_DEF_LVL2_REQ + ISAL_DEF_LVL2_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL2_DEFAULT ISAL_DEF_LVL2_LARGE

#define ISAL_DEF_LVL3_MIN                                                      \
  (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 1 * IGZIP_K)
#define ISAL_DEF_LVL3_SMALL                                                    \
  (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 16 * IGZIP_K)
#define ISAL_DEF_LVL3_MEDIUM                                                   \
  (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 32 * IGZIP_K)
#define ISAL_DEF_LVL3_LARGE                                                    \
  (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 64 * IGZIP_K)
#define ISAL_DEF_LVL3_EXTRA_LARGE                                              \
  (ISAL_DEF_LVL3_REQ + ISAL_DEF_LVL3_TOKEN_SIZE * 128 * IGZIP_K)
#define ISAL_DEF_LVL3_DEFAULT ISAL_DEF_LVL3_LARGE

#define IGZIP_NO_HIST 0
#define IGZIP_HIST 1
#define IGZIP_DICT_HIST 2
#define IGZIP_DICT_HASH_SET 3

struct BitBuf2 {
  uint64_t m_bits;
  uint32_t m_bit_count;
  uint8_t *m_out_buf;
  uint8_t *m_out_end;
  uint8_t *m_out_start;
};

struct isal_zlib_header {
  uint32_t info;
  uint32_t level;
  uint32_t dict_id;
  uint32_t dict_flag;
};

struct isal_gzip_header {
  uint32_t text;
  uint32_t time;
  uint32_t xflags;
  uint32_t os;
  uint8_t *extra;
  uint32_t extra_buf_len;
  uint32_t extra_len;
  char *name;
  uint32_t name_buf_len;
  char *comment;
  uint32_t comment_buf_len;
  uint32_t hcrc;
  uint32_t flags;
};

struct isal_zstate {
  uint32_t total_in_start;
  uint32_t block_next;
  uint32_t block_end;
  uint32_t dist_mask;
  uint32_t hash_mask;
  enum isal_zstate_state state;
  struct BitBuf2 bitbuf;
  uint32_t crc;
  uint8_t has_wrap_hdr;
  uint8_t has_eob_hdr;
  uint8_t has_eob;
  uint8_t has_hist;
  uint16_t has_level_buf_init;

  uint32_t count;
  uint8_t tmp_out_buff[16];
  uint32_t tmp_out_start;
  uint32_t tmp_out_end;
  uint32_t b_bytes_valid;
  uint32_t b_bytes_processed;
  uint8_t buffer[2 * IGZIP_HIST_SIZE + ISAL_LOOK_AHEAD];

  uint16_t head[IGZIP_LVL0_HASH_SIZE];
};

struct isal_hufftables {

  uint8_t deflate_hdr[ISAL_DEF_MAX_HDR_SIZE];
  uint32_t deflate_hdr_count;
  uint32_t deflate_hdr_extra_bits;
  uint32_t dist_table[IGZIP_DIST_TABLE_SIZE];

  uint32_t len_table[IGZIP_LEN_TABLE_SIZE];

  uint16_t lit_table[IGZIP_LIT_TABLE_SIZE];
  uint8_t lit_table_sizes[IGZIP_LIT_TABLE_SIZE];
  uint16_t dcodes[30 - IGZIP_DECODE_OFFSET];
  uint8_t dcodes_sizes[30 - IGZIP_DECODE_OFFSET];
};

struct isal_zstream {
  uint8_t *next_in;
  uint32_t avail_in;
  uint32_t total_in;

  uint8_t *next_out;
  uint32_t avail_out;
  uint32_t total_out;

  struct isal_hufftables *hufftables;
  uint32_t level;
  uint32_t level_buf_size;
  uint8_t *level_buf;

  uint16_t end_of_stream;
  uint16_t flush;
  uint16_t gzip_flag;
  uint16_t hist_bits;
  struct isal_zstate internal_state;
};

#define ISAL_L_REM (21 - ISAL_DECODE_LONG_BITS)
#define ISAL_S_REM (15 - ISAL_DECODE_SHORT_BITS)

#define ISAL_L_DUP ((1 << ISAL_L_REM) - (ISAL_L_REM + 1))
#define ISAL_S_DUP ((1 << ISAL_S_REM) - (ISAL_S_REM + 1))

#define ISAL_L_UNUSED                                                          \
  ((1 << ISAL_L_REM) - (1 << ((ISAL_L_REM) / 2)) -                             \
   (1 << ((ISAL_L_REM + 1) / 2)) + 1)
#define ISAL_S_UNUSED                                                          \
  ((1 << ISAL_S_REM) - (1 << ((ISAL_S_REM) / 2)) -                             \
   (1 << ((ISAL_S_REM + 1) / 2)) + 1)

#define ISAL_L_SIZE (ISAL_DEF_LIT_LEN_SYMBOLS + ISAL_L_DUP + ISAL_L_UNUSED)
#define ISAL_S_SIZE (ISAL_DEF_DIST_SYMBOLS + ISAL_S_DUP + ISAL_S_UNUSED)

#define ISAL_HUFF_CODE_LARGE_LONG_ALIGNED (ISAL_L_SIZE + (-ISAL_L_SIZE & 0xf))
#define ISAL_HUFF_CODE_SMALL_LONG_ALIGNED (ISAL_S_SIZE + (-ISAL_S_SIZE & 0xf))

struct inflate_huff_code_large {
  uint32_t short_code_lookup[1 << (ISAL_DECODE_LONG_BITS)];

  uint16_t long_code_lookup[ISAL_HUFF_CODE_LARGE_LONG_ALIGNED];
};

struct inflate_huff_code_small {
  uint16_t short_code_lookup[1 << (ISAL_DECODE_SHORT_BITS)];

  uint16_t long_code_lookup[ISAL_HUFF_CODE_SMALL_LONG_ALIGNED];
};

struct inflate_state {
  uint8_t *next_out;
  uint32_t avail_out;
  uint32_t total_out;
  uint8_t *next_in;
  uint64_t read_in;
  uint32_t avail_in;
  int32_t read_in_length;
  struct inflate_huff_code_large lit_huff_code;
  struct inflate_huff_code_small dist_huff_code;
  enum isal_block_state block_state;
  uint32_t dict_length;
  uint32_t bfinal;
  uint32_t crc_flag;
  uint32_t crc;
  uint32_t hist_bits;
  union {
    int32_t type0_block_len;

    int32_t count;
    uint32_t dict_id;
  };
  int32_t write_overflow_lits;
  int32_t write_overflow_len;
  int32_t copy_overflow_length;

  int32_t copy_overflow_distance;

  int16_t wrapper_flag;
  int16_t tmp_in_size;
  int32_t tmp_out_valid;
  int32_t tmp_out_processed;
  uint8_t tmp_in_buffer[ISAL_DEF_MAX_HDR_SIZE];

  uint8_t tmp_out_buffer[2 * ISAL_DEF_HIST_SIZE + ISAL_LOOK_AHEAD];

  enum isal_stopping_point points_to_stop_at;
  enum isal_stopping_point stopped_at;
  enum isal_stopping_point tmp_out_stopped_at;

  uint8_t btype;
};

void isal_update_histogram(uint8_t *in_stream, int length,
                           struct isal_huff_histogram *histogram);

int isal_create_hufftables(struct isal_hufftables *hufftables,
                           struct isal_huff_histogram *histogram);

int isal_create_hufftables_subset(struct isal_hufftables *hufftables,
                                  struct isal_huff_histogram *histogram);

void isal_deflate_init(struct isal_zstream *stream);

void isal_deflate_reset(struct isal_zstream *stream);

void isal_gzip_header_init(struct isal_gzip_header *gz_hdr);

void isal_zlib_header_init(struct isal_zlib_header *z_hdr);

uint32_t isal_write_gzip_header(struct isal_zstream *stream,
                                struct isal_gzip_header *gz_hdr);

uint32_t isal_write_zlib_header(struct isal_zstream *stream,
                                struct isal_zlib_header *z_hdr);

int isal_deflate_set_hufftables(struct isal_zstream *stream,
                                struct isal_hufftables *hufftables, int type);

void isal_deflate_stateless_init(struct isal_zstream *stream);

int isal_deflate_set_dict(struct isal_zstream *stream, uint8_t *dict,
                          uint32_t dict_len);

struct isal_dict {
  uint32_t params;
  uint32_t level;
  uint32_t hist_size;
  uint32_t hash_size;
  uint8_t history[ISAL_DEF_HIST_SIZE];
  uint16_t hashtable[IGZIP_LVL3_HASH_SIZE];
};

int isal_deflate_process_dict(struct isal_zstream *stream,
                              struct isal_dict *dict_str, uint8_t *dict,
                              uint32_t dict_len);

int isal_deflate_reset_dict(struct isal_zstream *stream,
                            struct isal_dict *dict_str);

int isal_deflate(struct isal_zstream *stream);

int isal_deflate_stateless(struct isal_zstream *stream);

void isal_inflate_init(struct inflate_state *state);

void isal_inflate_reset(struct inflate_state *state);

int isal_inflate_set_dict(struct inflate_state *state, const uint8_t *dict,
                          uint32_t dict_len);

int isal_read_gzip_header(struct inflate_state *state,
                          struct isal_gzip_header *gz_hdr);

int isal_read_zlib_header(struct inflate_state *state,
                          struct isal_zlib_header *zlib_hdr);

int isal_inflate(struct inflate_state *state);

int isal_inflate_stateless(struct inflate_state *state);

uint32_t isal_adler32(uint32_t init, const unsigned char *buf, uint64_t len);

struct huff_code {
  union {
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      uint32_t code_and_extra : 24;
      uint32_t length2 : 8;
#else
      uint32_t length2 : 8;
      uint32_t code_and_extra : 24;
#endif
    };

    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      uint16_t code;
      uint8_t extra_bit_count;
      uint8_t length;
#else
      uint8_t length;
      uint8_t extra_bit_count;
      uint16_t code;
#endif
    };

    uint32_t code_and_length;
  };
};

int set_and_expand_lit_len_huffcode(struct huff_code *const lit_len_huff,
                                    uint32_t const table_length,
                                    uint16_t *const count,
                                    uint16_t *const expand_count,
                                    uint32_t *const code_list);

void make_inflate_huff_code_lit_len(
    struct inflate_huff_code_large *const result,
    struct huff_code *const huff_code_table, uint32_t const,
    uint16_t const *const count_total, uint32_t *const code_list,
    uint32_t const multisym);

int set_codes(struct huff_code *huff_code_table, int const table_length,
              uint16_t const *const count);

void make_inflate_huff_code_dist(struct inflate_huff_code_small *const result,
                                 struct huff_code *const huff_code_table,
                                 uint32_t const table_length,
                                 uint16_t const *const count,
                                 uint32_t const max_symbol);
#ifdef __cplusplus
}
#endif
#endif
