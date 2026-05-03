/* deflate.c -- compress data using the deflation algorithm
 * Copyright (C) 1995-2023 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/*
 *  ALGORITHM
 *
 *      The "deflation" process depends on being able to identify portions
 *      of the input text which are identical to earlier input (within a
 *      sliding window trailing behind the input currently being processed).
 *
 *      The most straightforward technique turns out to be the fastest for
 *      most input files: try all possible matches and select the longest.
 *      The key feature of this algorithm is that insertions into the string
 *      dictionary are very simple and thus fast, and deletions are avoided
 *      completely. Insertions are performed at each input character, whereas
 *      string matches are performed only when the previous match ends. So it
 *      is preferable to spend more time in matches to allow very fast string
 *      insertions and avoid deletions. The matching algorithm for small
 *      strings is inspired from that of Rabin & Karp. A brute force approach
 *      is used to find longer strings when a small match has been found.
 *      A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 *      (by Leonid Broukhis).
 *         A previous version of this file used a more sophisticated algorithm
 *      (by Fiala and Greene) which is guaranteed to run in linear amortized
 *      time, but has a larger average cost, uses more memory and is patented.
 *      However the F&G algorithm may be faster for some highly redundant
 *      files if the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 *      The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 *      I found it in 'freeze' written by Leonid Broukhis.
 *      Thanks to many people for bug reports and testing.
 *
 *  REFERENCES
 *
 *      Deutsch, L.P.,"DEFLATE Compressed Data Format Specification".
 *      Available in http://tools.ietf.org/html/rfc1951
 *
 *      A description of the Rabin and Karp algorithm is given in the book
 *         "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 *      Fiala,E.R., and Greene,D.H.
 *         Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 */

#include "deflate.h"

const char deflate_copyright[] =
    " deflate 1.3 Copyright 1995-2023 Jean-loup Gailly and Mark Adler ";

typedef enum { need_more, block_done, finish_started, finish_done } block_state;

typedef block_state (*compress_func)(deflate_state *s, int flush);

static int deflateStateCheck(z_streamp strm);
static void fill_window(deflate_state *s);
static block_state deflate_stored(deflate_state *s, int flush);
static block_state deflate_fast(deflate_state *s, int flush);
static block_state deflate_slow(deflate_state *s, int flush);
static block_state deflate_rle(deflate_state *s, int flush);
static block_state deflate_huff(deflate_state *s, int flush);
static void lm_init(deflate_state *s);
static void putShortMSB(deflate_state *s, uint32_t b);
static void flush_pending(z_streamp strm);
static int read_buf(z_streamp strm, uint8_t *buf, uint32_t size);

#ifdef ZLIB_DEBUG
static void check_match(deflate_state *s, IPos start, IPos match, int length);
#endif

#define NIL 0

#define ACTUAL_MIN_MATCH 4

typedef struct config_s {
  uint16_t good_length;
  uint16_t max_lazy;
  uint16_t nice_length;
  uint16_t max_chain;
  compress_func func;
} config;

static const config configuration_table[10] = {

    {0, 0, 0, 0, deflate_stored},       {4, 4, 8, 4, deflate_fast},
    {4, 5, 16, 8, deflate_fast},        {4, 6, 32, 32, deflate_fast},

    {4, 4, 16, 16, deflate_slow},       {8, 16, 32, 32, deflate_slow},
    {8, 16, 128, 128, deflate_slow},    {8, 32, 128, 256, deflate_slow},
    {32, 128, 258, 1024, deflate_slow}, {32, 258, 258, 4096, deflate_slow}};

#define RANK(f) (((f) * 2) - ((f) > 4 ? 9 : 0))

#ifdef __aarch64__

#include <arm_acle.h>
#include <arm_neon.h>
static uint32_t hash_func(deflate_state *s, void *str) {
  return __crc32cw(0, *(uint32_t *)str) & s->hash_mask;
}

#elif defined HAS_SSE42

#include <immintrin.h>
static uint32_t hash_func(deflate_state *s, void *str) {
  return _mm_crc32_u32(0, *(uint32_t *)str) & s->hash_mask;
}

#else

static uint32_t hash_func(deflate_state *s, void *str) {
  uint32_t w;
  zmemcpy(&w, str, sizeof(w));

  w *= 0x85ebca77u;
  w ^= w >> 19;
  return w & s->hash_mask;
}

#endif

static Pos insert_string(deflate_state *s, Pos str) {
  Pos match_head;
  s->ins_h = hash_func(s, &s->window[str]);
  match_head = s->prev[(str)&s->w_mask] = s->head[s->ins_h];
  s->head[s->ins_h] = (Pos)str;
  return match_head;
}

static void bulk_insert_str(deflate_state *s, Pos startpos, uint32_t count) {
  uint32_t idx;
  for (idx = 0; idx < count; idx++) {
    s->ins_h = hash_func(s, &s->window[startpos + idx]);
    s->prev[(startpos + idx) & s->w_mask] = s->head[s->ins_h];
    s->head[s->ins_h] = (Pos)(startpos + idx);
  }
}

#define CLEAR_HASH(s)                                                          \
  zmemzero((uint8_t *)s->head, (unsigned)(s->hash_size) * sizeof(*s->head));

int ZEXPORT deflateInit_(z_streamp strm, int level, const char *version,
                         int stream_size) {
  return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, DEF_MEM_LEVEL,
                       Z_DEFAULT_STRATEGY, version, stream_size);
}

int ZEXPORT deflateInit2_(z_streamp strm, int level, int method, int windowBits,
                          int memLevel, int strategy, const char *version,
                          int stream_size) {
  deflate_state *s;
  int wrap = 1;
  static const char my_version[] = ZLIB_VERSION;

  if (version == Z_NULL || version[0] != my_version[0] ||
      stream_size != sizeof(z_stream)) {
    return Z_VERSION_ERROR;
  }
  if (strm == Z_NULL)
    return Z_STREAM_ERROR;

  strm->msg = Z_NULL;
  if (strm->zalloc == (alloc_func)0) {
#ifdef Z_SOLO
    return Z_STREAM_ERROR;
#else
    strm->zalloc = zcalloc;
    strm->opaque = (voidpf)0;
#endif
  }
  if (strm->zfree == (free_func)0)
#ifdef Z_SOLO
    return Z_STREAM_ERROR;
#else
    strm->zfree = zcfree;
#endif

  if (level == Z_DEFAULT_COMPRESSION)
    level = 6;

  if (windowBits < 0) {
    wrap = 0;
    if (windowBits < -15)
      return Z_STREAM_ERROR;
    windowBits = -windowBits;
  } else if (windowBits > 15) {
    wrap = 2;
    windowBits -= 16;
  }
  if (memLevel < 1 || memLevel > MAX_MEM_LEVEL || method != Z_DEFLATED ||
      windowBits < 8 || windowBits > 15 || level < 0 || level > 9 ||
      strategy < 0 || strategy > Z_FIXED || (windowBits == 8 && wrap != 1)) {
    return Z_STREAM_ERROR;
  }
  if (windowBits == 8)
    windowBits = 9;
  s = (deflate_state *)ZALLOC(strm, 1, sizeof(deflate_state));
  if (s == Z_NULL)
    return Z_MEM_ERROR;
  strm->state = (struct internal_state *)s;
  s->strm = strm;
  s->status = INIT_STATE;

  s->wrap = wrap;
  s->gzhead = Z_NULL;
  s->w_bits = windowBits;
  s->w_size = 1 << s->w_bits;
  s->w_mask = s->w_size - 1;

  s->hash_bits = memLevel + 7;
  s->hash_size = 1 << s->hash_bits;
  s->hash_mask = s->hash_size - 1;

  s->window = (uint8_t *)ZALLOC(strm, s->w_size, 2 * sizeof(uint8_t));
  s->prev = (Pos *)ZALLOC(strm, s->w_size, sizeof(Pos));
  s->head = (Pos *)ZALLOC(strm, s->hash_size, sizeof(Pos));

  s->high_water = 0;

  s->lit_bufsize = 1 << (memLevel + 6);

  s->pending_buf = (uint8_t *)ZALLOC(strm, s->lit_bufsize, 4);
  s->pending_buf_size = (uint64_t)s->lit_bufsize * 4;

  if (s->window == Z_NULL || s->prev == Z_NULL || s->head == Z_NULL ||
      s->pending_buf == Z_NULL) {
    s->status = FINISH_STATE;
    strm->msg = ERR_MSG(Z_MEM_ERROR);
    deflateEnd(strm);
    return Z_MEM_ERROR;
  }
  s->sym_buf = s->pending_buf + s->lit_bufsize;
  s->sym_end = (s->lit_bufsize - 1) * 3;

  s->level = level;
  s->strategy = strategy;
  s->method = (uint8_t)method;

  return deflateReset(strm);
}

static int deflateStateCheck(z_streamp strm) {
  deflate_state *s;
  if (strm == Z_NULL || strm->zalloc == (alloc_func)0 ||
      strm->zfree == (free_func)0)
    return 1;
  s = strm->state;
  if (s == Z_NULL || s->strm != strm ||
      (s->status != INIT_STATE && s->status != EXTRA_STATE &&
       s->status != NAME_STATE && s->status != COMMENT_STATE &&
       s->status != HCRC_STATE && s->status != BUSY_STATE &&
       s->status != FINISH_STATE))
    return 1;
  return 0;
}

int ZEXPORT deflateSetDictionary(z_streamp strm, const uint8_t *dictionary,
                                 uint32_t dictLength) {
  deflate_state *s;
  uint32_t str, n;
  int wrap;
  uint32_t avail;
  z_const uint8_t *next;

  if (deflateStateCheck(strm) || dictionary == Z_NULL)
    return Z_STREAM_ERROR;
  s = strm->state;
  wrap = s->wrap;
  if (wrap == 2 || (wrap == 1 && s->status != INIT_STATE) || s->lookahead)
    return Z_STREAM_ERROR;

  if (wrap == 1)
    strm->adler = adler32(strm->adler, dictionary, dictLength);
  s->wrap = 0;

  if (dictLength >= s->w_size) {
    if (wrap == 0) {
      CLEAR_HASH(s);
      s->strstart = 0;
      s->block_start = 0L;
      s->insert = 0;
    }
    dictionary += dictLength - s->w_size;
    dictLength = s->w_size;
  }

  avail = strm->avail_in;
  next = strm->next_in;
  strm->avail_in = dictLength;
  strm->next_in = (z_const uint8_t *)dictionary;
  fill_window(s);
  while (s->lookahead >= ACTUAL_MIN_MATCH) {
    str = s->strstart;
    n = s->lookahead - (ACTUAL_MIN_MATCH - 1);
    bulk_insert_str(s, str, n);
    s->strstart = str + n;
    s->lookahead = ACTUAL_MIN_MATCH - 1;
    fill_window(s);
  }
  s->strstart += s->lookahead;
  s->block_start = (long)s->strstart;
  s->insert = s->lookahead;
  s->lookahead = 0;
  s->match_length = s->prev_length = ACTUAL_MIN_MATCH - 1;
  s->match_available = 0;
  strm->next_in = next;
  strm->avail_in = avail;
  s->wrap = wrap;
  return Z_OK;
}

int ZEXPORT deflateResetKeep(z_streamp strm) {
  deflate_state *s;

  if (deflateStateCheck(strm)) {
    return Z_STREAM_ERROR;
  }

  strm->total_in = strm->total_out = 0;
  strm->msg = Z_NULL;
  strm->data_type = Z_UNKNOWN;

  s = (deflate_state *)strm->state;
  s->pending = 0;
  s->pending_out = s->pending_buf;

  if (s->wrap < 0) {
    s->wrap = -s->wrap;
  }
  s->status = s->wrap ? INIT_STATE : BUSY_STATE;
  strm->adler = s->wrap == 2 ? crc32(0L, Z_NULL, 0) : adler32(0L, Z_NULL, 0);
  s->last_flush = -2;

  _tr_init(s);

  return Z_OK;
}

int ZEXPORT deflateReset(z_streamp strm) {
  int ret;

  ret = deflateResetKeep(strm);
  if (ret == Z_OK)
    lm_init(strm->state);
  return ret;
}

int ZEXPORT deflateSetHeader(z_streamp strm, gz_headerp head) {
  if (deflateStateCheck(strm) || strm->state->wrap != 2)
    return Z_STREAM_ERROR;
  strm->state->gzhead = head;
  return Z_OK;
}

int ZEXPORT deflatePending(z_streamp strm, unsigned *pending, int *bits) {
  if (deflateStateCheck(strm))
    return Z_STREAM_ERROR;
  if (pending != Z_NULL)
    *pending = strm->state->pending;
  if (bits != Z_NULL)
    *bits = strm->state->bi_valid;
  return Z_OK;
}

int ZEXPORT deflatePrime(z_streamp strm, int bits, int value) {
  deflate_state *s;
  int put;

  if (deflateStateCheck(strm))
    return Z_STREAM_ERROR;
  s = strm->state;
  if (bits < 0 || bits > 16)
    return Z_BUF_ERROR;
  if ((uint8_t *)(s->sym_buf) < s->pending_out + ((Buf_size + 7) >> 3))
    return Z_BUF_ERROR;
  do {
    put = Buf_size - s->bi_valid;
    if (put > bits)
      put = bits;
    s->bi_buf |= (uint16_t)((value & ((1 << put) - 1)) << s->bi_valid);
    s->bi_valid += put;
    _tr_flush_bits(s);
    value >>= put;
    bits -= put;
  } while (bits);
  return Z_OK;
}

int ZEXPORT deflateParams(z_streamp strm, int level, int strategy) {
  deflate_state *s;
  compress_func func;

  if (deflateStateCheck(strm))
    return Z_STREAM_ERROR;
  s = strm->state;

  if (level == Z_DEFAULT_COMPRESSION)
    level = 6;
  if (level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
    return Z_STREAM_ERROR;
  }
  func = configuration_table[s->level].func;

  if ((strategy != s->strategy || func != configuration_table[level].func) &&
      s->last_flush != -2) {

    int err = deflate(strm, Z_BLOCK);
    if (err == Z_STREAM_ERROR)
      return err;
    if (strm->avail_in || (s->strstart - s->block_start) + s->lookahead)
      return Z_BUF_ERROR;
  }
  if (s->level != level) {
    s->level = level;
    s->max_lazy_match = configuration_table[level].max_lazy;
    s->good_match = configuration_table[level].good_length;
    s->nice_match = configuration_table[level].nice_length;
    s->max_chain_length = configuration_table[level].max_chain;
  }
  s->strategy = strategy;
  return Z_OK;
}

int ZEXPORT deflateTune(z_streamp strm, int good_length, int max_lazy,
                        int nice_length, int max_chain) {
  deflate_state *s;

  if (deflateStateCheck(strm))
    return Z_STREAM_ERROR;
  s = strm->state;
  s->good_match = good_length;
  s->max_lazy_match = max_lazy;
  s->nice_match = nice_length;
  s->max_chain_length = max_chain;
  return Z_OK;
}

uint64_t ZEXPORT deflateBound(z_streamp strm, uint64_t sourceLen) {
  deflate_state *s;
  uint64_t complen, wraplen;
  uint8_t *str;

  complen = sourceLen + ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5;

  if (deflateStateCheck(strm))
    return complen + 6;

  s = strm->state;
  switch (s->wrap) {
  case 0:
    wraplen = 0;
    break;
  case 1:
    wraplen = 6 + (s->strstart ? 4 : 0);
    break;
  case 2:
    wraplen = 18;
    if (s->gzhead != Z_NULL) {
      if (s->gzhead->extra != Z_NULL)
        wraplen += 2 + s->gzhead->extra_len;
      str = s->gzhead->name;
      if (str != Z_NULL)
        do {
          wraplen++;
        } while (*str++);
      str = s->gzhead->comment;
      if (str != Z_NULL)
        do {
          wraplen++;
        } while (*str++);
      if (s->gzhead->hcrc)
        wraplen += 2;
    }
    break;
  default:
    wraplen = 6;
  }

  if (s->w_bits != 15 || s->hash_bits != 8 + 7)
    return complen + wraplen;

  return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) +
         13 - 6 + wraplen;
}

static void putShortMSB(deflate_state *s, uint32_t b) {
  put_byte(s, (uint8_t)(b >> 8));
  put_byte(s, (uint8_t)(b & 0xff));
}

static void flush_pending(z_streamp strm) {
  uint32_t len;
  deflate_state *s = strm->state;

  _tr_flush_bits(s);
  len = s->pending;
  if (len > strm->avail_out)
    len = strm->avail_out;
  if (len == 0)
    return;

  zmemcpy(strm->next_out, s->pending_out, len);
  strm->next_out += len;
  s->pending_out += len;
  strm->total_out += len;
  strm->avail_out -= len;
  s->pending -= len;
  if (s->pending == 0) {
    s->pending_out = s->pending_buf;
  }
}

int ZEXPORT deflate(z_streamp strm, int flush) {
  int old_flush;
  deflate_state *s;

  if (deflateStateCheck(strm) || flush > Z_BLOCK || flush < 0) {
    return Z_STREAM_ERROR;
  }
  s = strm->state;

  if (strm->next_out == Z_NULL ||
      (strm->avail_in != 0 && strm->next_in == Z_NULL) ||
      (s->status == FINISH_STATE && flush != Z_FINISH)) {
    ERR_RETURN(strm, Z_STREAM_ERROR);
  }
  if (strm->avail_out == 0)
    ERR_RETURN(strm, Z_BUF_ERROR);

  s->strm = strm;
  old_flush = s->last_flush;
  s->last_flush = flush;

  if (s->status == INIT_STATE) {
    if (s->wrap == 2) {
      strm->adler = crc32(0L, Z_NULL, 0);
      put_byte(s, 31);
      put_byte(s, 139);
      put_byte(s, 8);
      if (s->gzhead == Z_NULL) {
        put_byte(s, 0);
        put_byte(s, 0);
        put_byte(s, 0);
        put_byte(s, 0);
        put_byte(s, 0);
        put_byte(s,
                 s->level == 9
                     ? 2
                     : (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
        put_byte(s, OS_CODE);
        s->status = BUSY_STATE;
      } else {
        put_byte(s, (s->gzhead->text ? 1 : 0) + (s->gzhead->hcrc ? 2 : 0) +
                        (s->gzhead->extra == Z_NULL ? 0 : 4) +
                        (s->gzhead->name == Z_NULL ? 0 : 8) +
                        (s->gzhead->comment == Z_NULL ? 0 : 16));
        put_byte(s, (uint8_t)(s->gzhead->time & 0xff));
        put_byte(s, (uint8_t)((s->gzhead->time >> 8) & 0xff));
        put_byte(s, (uint8_t)((s->gzhead->time >> 16) & 0xff));
        put_byte(s, (uint8_t)((s->gzhead->time >> 24) & 0xff));
        put_byte(s,
                 s->level == 9
                     ? 2
                     : (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
        put_byte(s, s->gzhead->os & 0xff);
        if (s->gzhead->extra != Z_NULL) {
          put_byte(s, s->gzhead->extra_len & 0xff);
          put_byte(s, (s->gzhead->extra_len >> 8) & 0xff);
        }
        if (s->gzhead->hcrc)
          strm->adler = crc32(strm->adler, s->pending_buf, s->pending);
        s->gzindex = 0;
        s->status = EXTRA_STATE;
      }
    } else {
      uint32_t header = (Z_DEFLATED + ((s->w_bits - 8) << 4)) << 8;
      uint32_t level_flags;

      if (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2)
        level_flags = 0;
      else if (s->level < 6)
        level_flags = 1;
      else if (s->level == 6)
        level_flags = 2;
      else
        level_flags = 3;
      header |= (level_flags << 6);
      if (s->strstart != 0)
        header |= PRESET_DICT;
      header += 31 - (header % 31);

      s->status = BUSY_STATE;
      putShortMSB(s, header);

      if (s->strstart != 0) {
        putShortMSB(s, (uint32_t)(strm->adler >> 16));
        putShortMSB(s, (uint32_t)(strm->adler & 0xffff));
      }
      strm->adler = adler32(0L, Z_NULL, 0);
    }
  }
  if (s->status == EXTRA_STATE) {
    if (s->gzhead->extra != Z_NULL) {
      uint32_t beg = s->pending;

      while (s->gzindex < (s->gzhead->extra_len & 0xffff)) {
        if (s->pending == s->pending_buf_size) {
          if (s->gzhead->hcrc && s->pending > beg)
            strm->adler =
                crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
          flush_pending(strm);
          beg = s->pending;
          if (s->pending == s->pending_buf_size)
            break;
        }
        put_byte(s, s->gzhead->extra[s->gzindex]);
        s->gzindex++;
      }
      if (s->gzhead->hcrc && s->pending > beg)
        strm->adler =
            crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
      if (s->gzindex == s->gzhead->extra_len) {
        s->gzindex = 0;
        s->status = NAME_STATE;
      }
    } else
      s->status = NAME_STATE;
  }
  if (s->status == NAME_STATE) {
    if (s->gzhead->name != Z_NULL) {
      uint32_t beg = s->pending;
      int val;

      do {
        if (s->pending == s->pending_buf_size) {
          if (s->gzhead->hcrc && s->pending > beg)
            strm->adler =
                crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
          flush_pending(strm);
          beg = s->pending;
          if (s->pending == s->pending_buf_size) {
            val = 1;
            break;
          }
        }
        val = s->gzhead->name[s->gzindex++];
        put_byte(s, val);
      } while (val != 0);
      if (s->gzhead->hcrc && s->pending > beg)
        strm->adler =
            crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
      if (val == 0) {
        s->gzindex = 0;
        s->status = COMMENT_STATE;
      }
    } else
      s->status = COMMENT_STATE;
  }
  if (s->status == COMMENT_STATE) {
    if (s->gzhead->comment != Z_NULL) {
      uint32_t beg = s->pending;
      int val;

      do {
        if (s->pending == s->pending_buf_size) {
          if (s->gzhead->hcrc && s->pending > beg)
            strm->adler =
                crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
          flush_pending(strm);
          beg = s->pending;
          if (s->pending == s->pending_buf_size) {
            val = 1;
            break;
          }
        }
        val = s->gzhead->comment[s->gzindex++];
        put_byte(s, val);
      } while (val != 0);
      if (s->gzhead->hcrc && s->pending > beg)
        strm->adler =
            crc32(strm->adler, s->pending_buf + beg, s->pending - beg);
      if (val == 0)
        s->status = HCRC_STATE;
    } else
      s->status = HCRC_STATE;
  }
  if (s->status == HCRC_STATE) {
    if (s->gzhead->hcrc) {
      if (s->pending + 2 > s->pending_buf_size)
        flush_pending(strm);
      if (s->pending + 2 <= s->pending_buf_size) {
        put_byte(s, (uint8_t)(strm->adler & 0xff));
        put_byte(s, (uint8_t)((strm->adler >> 8) & 0xff));
        strm->adler = crc32(0L, Z_NULL, 0);
        s->status = BUSY_STATE;
      }
    } else
      s->status = BUSY_STATE;
  }

  if (s->pending != 0) {
    flush_pending(strm);
    if (strm->avail_out == 0) {

      s->last_flush = -1;
      return Z_OK;
    }

  } else if (strm->avail_in == 0 && RANK(flush) <= RANK(old_flush) &&
             flush != Z_FINISH) {
    ERR_RETURN(strm, Z_BUF_ERROR);
  }

  if (s->status == FINISH_STATE && strm->avail_in != 0) {
    ERR_RETURN(strm, Z_BUF_ERROR);
  }

  if (strm->avail_in != 0 || s->lookahead != 0 ||
      (flush != Z_NO_FLUSH && s->status != FINISH_STATE)) {
    block_state bstate;

    bstate = s->level == 0                   ? deflate_stored(s, flush)
             : s->strategy == Z_HUFFMAN_ONLY ? deflate_huff(s, flush)
             : s->strategy == Z_RLE
                 ? deflate_rle(s, flush)
                 : (*(configuration_table[s->level].func))(s, flush);

    if (bstate == finish_started || bstate == finish_done) {
      s->status = FINISH_STATE;
    }
    if (bstate == need_more || bstate == finish_started) {
      if (strm->avail_out == 0) {
        s->last_flush = -1;
      }
      return Z_OK;
    }
    if (bstate == block_done) {
      if (flush == Z_PARTIAL_FLUSH) {
        _tr_align(s);
      } else if (flush != Z_BLOCK) {
        _tr_stored_block(s, (uint8_t *)0, 0L, 0);

        if (flush == Z_FULL_FLUSH) {
          CLEAR_HASH(s);
          if (s->lookahead == 0) {
            s->strstart = 0;
            s->block_start = 0L;
            s->insert = 0;
          }
        }
      }
      flush_pending(strm);
      if (strm->avail_out == 0) {
        s->last_flush = -1;
        return Z_OK;
      }
    }
  }
  Assert(strm->avail_out > 0, "bug2");

  if (flush != Z_FINISH)
    return Z_OK;
  if (s->wrap <= 0)
    return Z_STREAM_END;

  if (s->wrap == 2) {
    put_byte(s, (uint8_t)(strm->adler & 0xff));
    put_byte(s, (uint8_t)((strm->adler >> 8) & 0xff));
    put_byte(s, (uint8_t)((strm->adler >> 16) & 0xff));
    put_byte(s, (uint8_t)((strm->adler >> 24) & 0xff));
    put_byte(s, (uint8_t)(strm->total_in & 0xff));
    put_byte(s, (uint8_t)((strm->total_in >> 8) & 0xff));
    put_byte(s, (uint8_t)((strm->total_in >> 16) & 0xff));
    put_byte(s, (uint8_t)((strm->total_in >> 24) & 0xff));
  } else {
    putShortMSB(s, (uint32_t)(strm->adler >> 16));
    putShortMSB(s, (uint32_t)(strm->adler & 0xffff));
  }
  flush_pending(strm);

  if (s->wrap > 0)
    s->wrap = -s->wrap;
  return s->pending != 0 ? Z_OK : Z_STREAM_END;
}

int ZEXPORT deflateEnd(z_streamp strm) {
  int status;

  if (deflateStateCheck(strm))
    return Z_STREAM_ERROR;

  status = strm->state->status;

  TRY_FREE(strm, strm->state->pending_buf);
  TRY_FREE(strm, strm->state->head);
  TRY_FREE(strm, strm->state->prev);
  TRY_FREE(strm, strm->state->window);

  ZFREE(strm, strm->state);
  strm->state = Z_NULL;

  return status == BUSY_STATE ? Z_DATA_ERROR : Z_OK;
}

int ZEXPORT deflateCopy(z_streamp dest, z_streamp source) {
  deflate_state *ds;
  deflate_state *ss;

  if (deflateStateCheck(source) || dest == Z_NULL) {
    return Z_STREAM_ERROR;
  }

  ss = source->state;

  zmemcpy((voidpf)dest, (voidpf)source, sizeof(z_stream));

  ds = (deflate_state *)ZALLOC(dest, 1, sizeof(deflate_state));
  if (ds == Z_NULL)
    return Z_MEM_ERROR;
  dest->state = (struct internal_state *)ds;
  zmemcpy((voidpf)ds, (voidpf)ss, sizeof(deflate_state));
  ds->strm = dest;

  ds->window = (uint8_t *)ZALLOC(dest, ds->w_size, 2 * sizeof(uint8_t));
  ds->prev = (Pos *)ZALLOC(dest, ds->w_size, sizeof(Pos));
  ds->head = (Pos *)ZALLOC(dest, ds->hash_size, sizeof(Pos));
  ds->pending_buf =
      (uint8_t *)ZALLOC(dest, ds->lit_bufsize, sizeof(uint16_t) + 2);

  if (ds->window == Z_NULL || ds->prev == Z_NULL || ds->head == Z_NULL ||
      ds->pending_buf == Z_NULL) {
    deflateEnd(dest);
    return Z_MEM_ERROR;
  }

  zmemcpy(ds->window, ss->window, ds->w_size * 2 * sizeof(uint8_t));
  zmemcpy((voidpf)ds->prev, (voidpf)ss->prev, ds->w_size * sizeof(Pos));
  zmemcpy((voidpf)ds->head, (voidpf)ss->head, ds->hash_size * sizeof(Pos));
  zmemcpy(ds->pending_buf, ss->pending_buf, (uint32_t)ds->pending_buf_size);

  ds->pending_out = ds->pending_buf + (ss->pending_out - ss->pending_buf);
  ds->sym_buf = ds->pending_buf + ds->lit_bufsize;

  ds->l_desc.dyn_tree = ds->dyn_ltree;
  ds->d_desc.dyn_tree = ds->dyn_dtree;
  ds->bl_desc.dyn_tree = ds->bl_tree;

  return Z_OK;
}

static int read_buf(z_streamp strm, uint8_t *buf, uint32_t size) {
  uint32_t len = strm->avail_in;

  if (len > size)
    len = size;
  if (len == 0)
    return 0;

  strm->avail_in -= len;

  zmemcpy(buf, strm->next_in, len);
  if (strm->state->wrap == 1) {
    strm->adler = adler32(strm->adler, buf, len);
  } else if (strm->state->wrap == 2) {
    strm->adler = crc32(strm->adler, buf, len);
  }
  strm->next_in += len;
  strm->total_in += len;

  return (int)len;
}

static void lm_init(deflate_state *s) {
  s->window_size = (uint64_t)2L * s->w_size;

  CLEAR_HASH(s);

  s->max_lazy_match = configuration_table[s->level].max_lazy;
  s->good_match = configuration_table[s->level].good_length;
  s->nice_match = configuration_table[s->level].nice_length;
  s->max_chain_length = configuration_table[s->level].max_chain;

  s->strstart = 0;
  s->block_start = 0L;
  s->lookahead = 0;
  s->insert = 0;
  s->match_length = s->prev_length = ACTUAL_MIN_MATCH - 1;
  s->match_available = 0;
  s->ins_h = 0;
}

static uint32_t longest_match(deflate_state *s, IPos cur_match) {
  uint32_t chain_length = s->max_chain_length;
  uint8_t *scan = s->window + s->strstart;
  uint8_t *match;
  int len;
  int best_len = (s->prev_length == 0) ? ACTUAL_MIN_MATCH - 1 : s->prev_length;
  int nice_match = s->nice_match;
  IPos limit =
      s->strstart > (IPos)MAX_DIST(s) ? s->strstart - (IPos)MAX_DIST(s) : NIL;

  Pos *prev = s->prev;
  uint32_t wmask = s->w_mask;

  uint8_t *strend = s->window + s->strstart + MAX_MATCH;

  uint32_t scan_start = *(uint32_t *)scan;
  uint32_t scan_end = *(uint32_t *)(scan + best_len - 3);

  Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");

  if (s->prev_length >= s->good_match) {
    chain_length >>= 2;
  }

  if ((uint32_t)nice_match > s->lookahead)
    nice_match = s->lookahead;

  Assert((uint64_t)s->strstart <= s->window_size - MIN_LOOKAHEAD,
         "need lookahead");

  do {
    int cont;
    Assert(cur_match < s->strstart, "no future");

    cont = 1;
    do {
      match = s->window + cur_match;
      if (likely(*(uint32_t *)(match + best_len - 3) != scan_end) ||
          (*(uint32_t *)match != scan_start)) {
        if ((cur_match = prev[cur_match & wmask]) > limit &&
            --chain_length != 0) {
          continue;
        } else
          cont = 0;
      }
      break;
    } while (1);

    if (!cont)
      break;

    scan += 4, match += 4;
    do {
      uint64_t sv = *(uint64_t *)(void *)scan;
      uint64_t mv = *(uint64_t *)(void *)match;
      uint64_t xval = sv ^ mv;
      if (xval) {
        int match_byte = __builtin_ctzl(xval) / 8;
        scan += match_byte;
        match += match_byte;
        break;
      } else {
        scan += 8;
        match += 8;
      }
    } while (scan < strend);

    if (scan > strend)
      scan = strend;

    Assert(scan <= s->window + (uint32_t)(s->window_size - 1), "wild scan");

    len = MAX_MATCH - (int)(strend - scan);
    scan = strend - MAX_MATCH;

    if (len > best_len) {
      s->match_start = cur_match;
      best_len = len;
      if (len >= nice_match)
        break;
      scan_end = *(uint32_t *)(scan + best_len - 3);
    }
  } while ((cur_match = prev[cur_match & wmask]) > limit &&
           --chain_length != 0);

  if ((uint32_t)best_len <= s->lookahead)
    return (uint32_t)best_len;
  return s->lookahead;
}

#ifdef ZLIB_DEBUG

#define EQUAL 0

static void check_match(deflate_state *s, IPos start, IPos match, int length) {

  if (zmemcmp(s->window + match, s->window + start, length) != EQUAL) {
    fprintf(stderr, " start %u, match %u, length %d\n", start, match, length);
    do {
      fprintf(stderr, "%c%c", s->window[match++], s->window[start++]);
    } while (--length != 0);
    z_error("invalid match");
  }
  if (z_verbose > 1) {
    fprintf(stderr, "\\[%d,%d]", start - match, length);
    do {
      putc(s->window[start++], stderr);
    } while (--length != 0);
  }
}
#else
#define check_match(s, start, match, length)
#endif

static void fill_window(deflate_state *s) {
  uint32_t n, m;
  Pos *p;
  uint32_t more;
  uint32_t wsize = s->w_size;

  Assert(s->lookahead < MIN_LOOKAHEAD, "already enough lookahead");

  do {
    more =
        (unsigned)(s->window_size - (uint64_t)s->lookahead - (ulg)s->strstart);

    if (sizeof(int) <= 2) {
      if (more == 0 && s->strstart == 0 && s->lookahead == 0) {
        more = wsize;

      } else if (more == (unsigned)(-1)) {

        more--;
      }
    }

    if (s->strstart >= wsize + MAX_DIST(s)) {

      int i;
      zmemcpy(s->window, s->window + wsize, (unsigned)wsize);
      s->match_start -= wsize;
      s->strstart -= wsize;
      s->block_start -= (int64_t)wsize;
      n = s->hash_size;

#ifdef __aarch64__

      uint16x8_t W;
      uint16_t *q;
      W = vmovq_n_u16(wsize);
      q = (uint16_t *)s->head;

      for (i = 0; i < n / 8; i++) {
        vst1q_u16(q, vqsubq_u16(vld1q_u16(q), W));
        q += 8;
      }

      n = wsize;
      q = (uint16_t *)s->prev;

      for (i = 0; i < n / 8; i++) {
        vst1q_u16(q, vqsubq_u16(vld1q_u16(q), W));
        q += 8;
      }

#elif defined HAS_SSE2

      __m128i W;
      __m128i *q;
      W = _mm_set1_epi16(wsize);
      q = (__m128i *)s->head;

      for (i = 0; i < n / 8; i++) {
        _mm_storeu_si128(q, _mm_subs_epu16(_mm_loadu_si128(q), W));
        q++;
      }

      n = wsize;
      q = (__m128i *)s->prev;

      for (i = 0; i < n / 8; i++) {
        _mm_storeu_si128(q, _mm_subs_epu16(_mm_loadu_si128(q), W));
        q++;
      }

#endif
      more += wsize;
    }
    if (s->strm->avail_in == 0)
      break;

    Assert(more >= 2, "more < 2");

    n = read_buf(s->strm, s->window + s->strstart + s->lookahead, more);
    s->lookahead += n;

    if (s->lookahead + s->insert >= ACTUAL_MIN_MATCH) {
      uint32_t str = s->strstart - s->insert;
      uint32_t ins_h = s->window[str];
      while (s->insert) {
        ins_h = hash_func(s, &s->window[str]);
        s->prev[str & s->w_mask] = s->head[ins_h];
        s->head[ins_h] = (Pos)str;
        str++;
        s->insert--;
        if (s->lookahead + s->insert < ACTUAL_MIN_MATCH)
          break;
      }
      s->ins_h = ins_h;
    }

  } while (s->lookahead < MIN_LOOKAHEAD && s->strm->avail_in != 0);

  if (s->high_water < s->window_size) {
    uint64_t curr = s->strstart + (ulg)(s->lookahead);
    uint64_t init;

    if (s->high_water < curr) {

      init = s->window_size - curr;
      if (init > WIN_INIT)
        init = WIN_INIT;
      zmemzero(s->window + curr, (unsigned)init);
      s->high_water = curr + init;
    } else if (s->high_water < (uint64_t)curr + WIN_INIT) {

      init = (uint64_t)curr + WIN_INIT - s->high_water;
      if (init > s->window_size - s->high_water)
        init = s->window_size - s->high_water;
      zmemzero(s->window + s->high_water, (unsigned)init);
      s->high_water += init;
    }
  }

  Assert((uint64_t)s->strstart <= s->window_size - MIN_LOOKAHEAD,
         "not enough room for search");
}

#define FLUSH_BLOCK_ONLY(s, last)                                              \
  {                                                                            \
    _tr_flush_block(s,                                                         \
                    (s->block_start >= 0L                                      \
                         ? (uint8_t *)&s->window[(uint64_t)s->block_start]     \
                         : (uint8_t *)Z_NULL),                                 \
                    (uint64_t)((int64_t)s->strstart - s->block_start),         \
                    (last));                                                   \
    s->block_start = s->strstart;                                              \
    flush_pending(s->strm);                                                    \
    Tracev((stderr, "[FLUSH]"));                                               \
  }

#define FLUSH_BLOCK(s, last)                                                   \
  {                                                                            \
    FLUSH_BLOCK_ONLY(s, last);                                                 \
    if (s->strm->avail_out == 0)                                               \
      return (last) ? finish_started : need_more;                              \
  }

static block_state deflate_stored(deflate_state *s, int flush) {

  uint64_t max_block_size = 0xffff;
  uint64_t max_start;

  if (max_block_size > s->pending_buf_size - 6) {
    max_block_size = s->pending_buf_size - 6;
  }

  for (;;) {

    if (s->lookahead <= 1) {

      Assert(s->strstart < s->w_size + MAX_DIST(s) ||
                 s->block_start >= (int64_t)s->w_size,
             "slide too late");

      fill_window(s);
      if (s->lookahead == 0 && flush == Z_NO_FLUSH)
        return need_more;

      if (s->lookahead == 0)
        break;
    }
    Assert(s->block_start >= 0L, "block gone");

    s->strstart += s->lookahead;
    s->lookahead = 0;

    max_start = s->block_start + max_block_size;
    if (s->strstart == 0 || (uint64_t)s->strstart >= max_start) {

      s->lookahead = (uint32_t)(s->strstart - max_start);
      s->strstart = (uint32_t)max_start;
      FLUSH_BLOCK(s, 0);
    }

    if (s->strstart - (uint32_t)s->block_start >= MAX_DIST(s)) {
      FLUSH_BLOCK(s, 0);
    }
  }
  s->insert = 0;
  if (flush == Z_FINISH) {
    FLUSH_BLOCK(s, 1);
    return finish_done;
  }
  if ((int64_t)s->strstart > s->block_start)
    FLUSH_BLOCK(s, 0);
  return block_done;
}

static block_state deflate_fast(deflate_state *s, int flush) {
  IPos hash_head;
  int bflush;

  for (;;) {

    if (s->lookahead < MIN_LOOKAHEAD) {
      fill_window(s);
      if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
        return need_more;
      }
      if (s->lookahead == 0)
        break;
    }

    hash_head = NIL;
    if (s->lookahead >= ACTUAL_MIN_MATCH) {
      hash_head = insert_string(s, s->strstart);
    }

    if (hash_head != NIL && s->strstart - hash_head <= MAX_DIST(s)) {

      s->match_length = longest_match(s, hash_head);
    }
    if (s->match_length >= ACTUAL_MIN_MATCH) {
      check_match(s, s->strstart, s->match_start, s->match_length);

      _tr_tally_dist(s, s->strstart - s->match_start,
                     s->match_length - MIN_MATCH, bflush);

      s->lookahead -= s->match_length;

      if (s->match_length <= s->max_insert_length &&
          s->lookahead >= ACTUAL_MIN_MATCH) {
        s->match_length--;
        do {
          s->strstart++;
          hash_head = insert_string(s, s->strstart);

        } while (--s->match_length != 0);
        s->strstart++;
      } else {
        s->strstart += s->match_length;
        s->match_length = 0;
      }
    } else {

      Tracevv((stderr, "%c", s->window[s->strstart]));
      _tr_tally_lit(s, s->window[s->strstart], bflush);
      s->lookahead--;
      s->strstart++;
    }
    if (bflush)
      FLUSH_BLOCK(s, 0);
  }
  s->insert =
      s->strstart < ACTUAL_MIN_MATCH - 1 ? s->strstart : ACTUAL_MIN_MATCH - 1;
  if (flush == Z_FINISH) {
    FLUSH_BLOCK(s, 1);
    return finish_done;
  }
  if (s->sym_next)
    FLUSH_BLOCK(s, 0);
  return block_done;
}

static block_state deflate_slow(deflate_state *s, int flush) {
  IPos hash_head;
  int bflush;

  for (;;) {

    if (s->lookahead < MIN_LOOKAHEAD) {
      fill_window(s);
      if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
        return need_more;
      }
      if (s->lookahead == 0)
        break;
    }

    hash_head = NIL;
    if (s->lookahead >= ACTUAL_MIN_MATCH) {
      hash_head = insert_string(s, s->strstart);
    }

    s->prev_length = s->match_length, s->prev_match = s->match_start;
    s->match_length = ACTUAL_MIN_MATCH - 1;

    if (hash_head != NIL && s->prev_length < s->max_lazy_match &&
        s->strstart - hash_head <= MAX_DIST(s)) {

      s->match_length = longest_match(s, hash_head);

      if (s->match_length <= 5 && (s->strategy == Z_FILTERED)) {

        s->match_length = ACTUAL_MIN_MATCH - 1;
      }
    }

    if (s->prev_length >= ACTUAL_MIN_MATCH &&
        s->match_length <= s->prev_length) {
      uint32_t mov_fwd;
      uint32_t insert_cnt;

      uint32_t max_insert = s->strstart + s->lookahead - ACTUAL_MIN_MATCH;

      check_match(s, s->strstart - 1, s->prev_match, s->prev_length);

      _tr_tally_dist(s, s->strstart - 1 - s->prev_match,
                     s->prev_length - MIN_MATCH, bflush);

      s->lookahead -= s->prev_length - 1;

      mov_fwd = s->prev_length - 2;
      insert_cnt = mov_fwd;
      if (unlikely(insert_cnt > max_insert - s->strstart))
        insert_cnt = max_insert - s->strstart;

      bulk_insert_str(s, s->strstart + 1, insert_cnt);
      s->prev_length = ACTUAL_MIN_MATCH - 1;
      s->match_available = 0;
      s->match_length = ACTUAL_MIN_MATCH - 1;
      s->strstart += mov_fwd + 1;

      if (bflush)
        FLUSH_BLOCK(s, 0);

    } else if (s->match_available) {

      Tracevv((stderr, "%c", s->window[s->strstart - 1]));
      _tr_tally_lit(s, s->window[s->strstart - 1], bflush);
      if (bflush) {
        FLUSH_BLOCK_ONLY(s, 0);
      }
      s->strstart++;
      s->lookahead--;
      if (s->strm->avail_out == 0)
        return need_more;
    } else {

      s->match_available = 1;
      s->strstart++;
      s->lookahead--;
    }
  }
  Assert(flush != Z_NO_FLUSH, "no flush?");
  if (s->match_available) {
    Tracevv((stderr, "%c", s->window[s->strstart - 1]));
    _tr_tally_lit(s, s->window[s->strstart - 1], bflush);
    s->match_available = 0;
  }
  s->insert =
      s->strstart < ACTUAL_MIN_MATCH - 1 ? s->strstart : ACTUAL_MIN_MATCH - 1;
  if (flush == Z_FINISH) {
    FLUSH_BLOCK(s, 1);
    return finish_done;
  }
  if (s->sym_next)
    FLUSH_BLOCK(s, 0);
  return block_done;
}

static block_state deflate_rle(deflate_state *s, int flush) {
  int bflush;
  uint32_t prev;
  uint8_t *scan, *strend;

  for (;;) {

    if (s->lookahead <= MAX_MATCH) {
      fill_window(s);
      if (s->lookahead <= MAX_MATCH && flush == Z_NO_FLUSH) {
        return need_more;
      }
      if (s->lookahead == 0)
        break;
    }

    s->match_length = 0;
    if (s->lookahead >= ACTUAL_MIN_MATCH && s->strstart > 0) {
      scan = s->window + s->strstart - 1;
      prev = *scan;
      if (prev == *++scan && prev == *++scan && prev == *++scan) {
        strend = s->window + s->strstart + MAX_MATCH;
        do {
        } while (prev == *++scan && prev == *++scan && prev == *++scan &&
                 prev == *++scan && prev == *++scan && prev == *++scan &&
                 prev == *++scan && prev == *++scan && scan < strend);
        s->match_length = MAX_MATCH - (int)(strend - scan);
        if (s->match_length > s->lookahead)
          s->match_length = s->lookahead;
      }
      Assert(scan <= s->window + (uint32_t)(s->window_size - 1), "wild scan");
    }

    if (s->match_length >= ACTUAL_MIN_MATCH) {
      check_match(s, s->strstart, s->strstart - 1, s->match_length);

      _tr_tally_dist(s, 1, s->match_length - MIN_MATCH, bflush);

      s->lookahead -= s->match_length;
      s->strstart += s->match_length;
      s->match_length = 0;
    } else {

      Tracevv((stderr, "%c", s->window[s->strstart]));
      _tr_tally_lit(s, s->window[s->strstart], bflush);
      s->lookahead--;
      s->strstart++;
    }
    if (bflush)
      FLUSH_BLOCK(s, 0);
  }
  s->insert = 0;
  if (flush == Z_FINISH) {
    FLUSH_BLOCK(s, 1);
    return finish_done;
  }
  if (s->sym_next)
    FLUSH_BLOCK(s, 0);
  return block_done;
}

static block_state deflate_huff(deflate_state *s, int flush) {
  int bflush;

  for (;;) {

    if (s->lookahead == 0) {
      fill_window(s);
      if (s->lookahead == 0) {
        if (flush == Z_NO_FLUSH)
          return need_more;
        break;
      }
    }

    s->match_length = 0;
    Tracevv((stderr, "%c", s->window[s->strstart]));
    _tr_tally_lit(s, s->window[s->strstart], bflush);
    s->lookahead--;
    s->strstart++;
    if (bflush)
      FLUSH_BLOCK(s, 0);
  }
  s->insert = 0;
  if (flush == Z_FINISH) {
    FLUSH_BLOCK(s, 1);
    return finish_done;
  }
  if (s->sym_next)
    FLUSH_BLOCK(s, 0);
  return block_done;
}
