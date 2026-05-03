/* chunkcopy.h -- fast chunk copy and set operations
 *
 * (C) 1995-2013 Jean-loup Gailly and Mark Adler
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Jean-loup Gailly        Mark Adler
 * jloup@gzip.org          madler@alumni.caltech.edu
 *
 * Copyright (C) 2017 ARM, Inc.
 * Copyright 2017 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 */

#ifndef CHUNKCOPY_H
#define CHUNKCOPY_H

#include "zutil.h"
#include <stdint.h>

#define Z_STATIC_ASSERT(name, assert) typedef char name[(assert) ? 1 : -1]

#if __STDC_VERSION__ >= 199901L
#define Z_RESTRICT restrict
#else
#define Z_RESTRICT
#endif

#if defined(__clang__) || defined(__GNUC__) || defined(__llvm__)
#define Z_BUILTIN_MEMCPY __builtin_memcpy
#define Z_BUILTIN_MEMSET __builtin_memset
#else
#define Z_BUILTIN_MEMCPY zmemcpy

#ifndef zmemset
#define zmemset memset
#endif
#define Z_BUILTIN_MEMSET zmemset
#endif

#if defined(INFLATE_CHUNK_SIMD_NEON)
#include <arm_neon.h>
typedef uint8x16_t z_vec128i_t;
#elif defined(INFLATE_CHUNK_SIMD_SSE2)
#include <emmintrin.h>
typedef __m128i z_vec128i_t;
#else
typedef struct {
  uint8_t x[16];
} z_vec128i_t;
#endif

#define CHUNKCOPY_CHUNK_SIZE sizeof(z_vec128i_t)

Z_STATIC_ASSERT(vector_128_bits_wide,
                CHUNKCOPY_CHUNK_SIZE == sizeof(int8_t) * 16);

static inline z_vec128i_t loadchunk(const unsigned char FAR *s) {
  z_vec128i_t v;
  Z_BUILTIN_MEMCPY(&v, s, sizeof(v));
  return v;
}

static inline void storechunk(unsigned char FAR *d, const z_vec128i_t v) {
  Z_BUILTIN_MEMCPY(d, &v, sizeof(v));
}

static inline unsigned char FAR *chunkcopy_core(unsigned char FAR *out,
                                                const unsigned char FAR *from,
                                                unsigned len) {
  const int bump = (--len % CHUNKCOPY_CHUNK_SIZE) + 1;
  storechunk(out, loadchunk(from));
  out += bump;
  from += bump;
  len /= CHUNKCOPY_CHUNK_SIZE;
  while (len-- > 0) {
    storechunk(out, loadchunk(from));
    out += CHUNKCOPY_CHUNK_SIZE;
    from += CHUNKCOPY_CHUNK_SIZE;
  }
  return out;
}

static inline unsigned char FAR *
chunkcopy_core_safe(unsigned char FAR *out, const unsigned char FAR *from,
                    unsigned len, unsigned char FAR *limit) {
  Assert(out + len <= limit, "chunk copy exceeds safety limit");
  if ((limit - out) < (ptrdiff_t)CHUNKCOPY_CHUNK_SIZE) {
    const unsigned char FAR *Z_RESTRICT rfrom = from;
    if (len & 8) {
      Z_BUILTIN_MEMCPY(out, rfrom, 8);
      out += 8;
      rfrom += 8;
    }
    if (len & 4) {
      Z_BUILTIN_MEMCPY(out, rfrom, 4);
      out += 4;
      rfrom += 4;
    }
    if (len & 2) {
      Z_BUILTIN_MEMCPY(out, rfrom, 2);
      out += 2;
      rfrom += 2;
    }
    if (len & 1) {
      *out++ = *rfrom++;
    }
    return out;
  }
  return chunkcopy_core(out, from, len);
}

static inline unsigned char FAR *chunkunroll_relaxed(unsigned char FAR *out,
                                                     unsigned FAR *dist,
                                                     unsigned FAR *len) {
  const unsigned char FAR *from = out - *dist;
  while (*dist < *len && *dist < CHUNKCOPY_CHUNK_SIZE) {
    storechunk(out, loadchunk(from));
    out += *dist;
    *len -= *dist;
    *dist += *dist;
  }
  return out;
}

#if defined(INFLATE_CHUNK_SIMD_NEON)

static inline z_vec128i_t v_load64_dup(const void *src) {
  return vcombine_u8(vld1_u8(src), vld1_u8(src));
}

static inline z_vec128i_t v_load32_dup(const void *src) {
  int32_t i32;
  Z_BUILTIN_MEMCPY(&i32, src, sizeof(i32));
  return vreinterpretq_u8_s32(vdupq_n_s32(i32));
}

static inline z_vec128i_t v_load16_dup(const void *src) {
  int16_t i16;
  Z_BUILTIN_MEMCPY(&i16, src, sizeof(i16));
  return vreinterpretq_u8_s16(vdupq_n_s16(i16));
}

static inline z_vec128i_t v_load8_dup(const void *src) {
  return vld1q_dup_u8((const uint8_t *)src);
}

static inline void v_store_128(void *out, const z_vec128i_t vec) {
  vst1q_u8(out, vec);
}
#elif defined(INFLATE_CHUNK_SIMD_SSE2)

static inline z_vec128i_t v_load64_dup(const void *src) {
  int64_t i64;
  Z_BUILTIN_MEMCPY(&i64, src, sizeof(i64));
  return _mm_set1_epi64x(i64);
}

static inline z_vec128i_t v_load32_dup(const void *src) {
  int32_t i32;
  Z_BUILTIN_MEMCPY(&i32, src, sizeof(i32));
  return _mm_set1_epi32(i32);
}

static inline z_vec128i_t v_load16_dup(const void *src) {
  int16_t i16;
  Z_BUILTIN_MEMCPY(&i16, src, sizeof(i16));
  return _mm_set1_epi16(i16);
}

static inline z_vec128i_t v_load8_dup(const void *src) {
  return _mm_set1_epi8(*(const char *)src);
}

static inline void v_store_128(void *out, const z_vec128i_t vec) {
  _mm_storeu_si128((__m128i *)out, vec);
}
#else

static inline z_vec128i_t v_load64_dup(const void *src) {
  int64_t in;
  Z_BUILTIN_MEMCPY(&in, src, sizeof(in));
  z_vec128i_t out;
  for (int i = 0; i < sizeof(out); i += sizeof(in)) {
    Z_BUILTIN_MEMCPY((uint8_t *)&out + i, &in, sizeof(in));
  }
  return out;
}

static inline z_vec128i_t v_load32_dup(const void *src) {
  int32_t in;
  Z_BUILTIN_MEMCPY(&in, src, sizeof(in));
  z_vec128i_t out;
  for (int i = 0; i < sizeof(out); i += sizeof(in)) {
    Z_BUILTIN_MEMCPY((uint8_t *)&out + i, &in, sizeof(in));
  }
  return out;
}

static inline z_vec128i_t v_load16_dup(const void *src) {
  int16_t in;
  Z_BUILTIN_MEMCPY(&in, src, sizeof(in));
  z_vec128i_t out;
  for (int i = 0; i < sizeof(out); i += sizeof(in)) {
    Z_BUILTIN_MEMCPY((uint8_t *)&out + i, &in, sizeof(in));
  }
  return out;
}

static inline z_vec128i_t v_load8_dup(const void *src) {
  int8_t in = *(uint8_t const *)src;
  z_vec128i_t out;
  Z_BUILTIN_MEMSET(&out, in, sizeof(out));
  return out;
}

static inline void v_store_128(void *out, const z_vec128i_t vec) {
  Z_BUILTIN_MEMCPY(out, &vec, sizeof(vec));
}
#endif

static inline unsigned char FAR *chunkset_core(unsigned char FAR *out,
                                               unsigned period, unsigned len) {
  z_vec128i_t v;
  const int bump = ((len - 1) % sizeof(v)) + 1;

  switch (period) {
  case 1:
    v = v_load8_dup(out - 1);
    v_store_128(out, v);
    out += bump;
    len -= bump;
    while (len > 0) {
      v_store_128(out, v);
      out += sizeof(v);
      len -= sizeof(v);
    }
    return out;
  case 2:
    v = v_load16_dup(out - 2);
    v_store_128(out, v);
    out += bump;
    len -= bump;
    if (len > 0) {
      v = v_load16_dup(out - 2);
      do {
        v_store_128(out, v);
        out += sizeof(v);
        len -= sizeof(v);
      } while (len > 0);
    }
    return out;
  case 4:
    v = v_load32_dup(out - 4);
    v_store_128(out, v);
    out += bump;
    len -= bump;
    if (len > 0) {
      v = v_load32_dup(out - 4);
      do {
        v_store_128(out, v);
        out += sizeof(v);
        len -= sizeof(v);
      } while (len > 0);
    }
    return out;
  case 8:
    v = v_load64_dup(out - 8);
    v_store_128(out, v);
    out += bump;
    len -= bump;
    if (len > 0) {
      v = v_load64_dup(out - 8);
      do {
        v_store_128(out, v);
        out += sizeof(v);
        len -= sizeof(v);
      } while (len > 0);
    }
    return out;
  }
  out = chunkunroll_relaxed(out, &period, &len);
  return chunkcopy_core(out, out - period, len);
}

static inline unsigned char FAR *
chunkcopy_relaxed(unsigned char FAR *Z_RESTRICT out,
                  const unsigned char FAR *Z_RESTRICT from, unsigned len) {
  return chunkcopy_core(out, from, len);
}

static inline unsigned char FAR *
chunkcopy_safe(unsigned char FAR *out, const unsigned char FAR *Z_RESTRICT from,
               unsigned len, unsigned char FAR *limit) {
  Assert(out + len <= limit, "chunk copy exceeds safety limit");
  return chunkcopy_core_safe(out, from, len, limit);
}

static inline unsigned char FAR *
chunkcopy_lapped_relaxed(unsigned char FAR *out, unsigned dist, unsigned len) {
  if (dist < len && dist < CHUNKCOPY_CHUNK_SIZE) {
    return chunkset_core(out, dist, len);
  }
  return chunkcopy_core(out, out - dist, len);
}

static inline unsigned char FAR *
chunkcopy_lapped_safe(unsigned char FAR *out, unsigned dist, unsigned len,
                      unsigned char FAR *limit) {
  Assert(out + len <= limit, "chunk copy exceeds safety limit");
  if ((limit - out) < (ptrdiff_t)(3 * CHUNKCOPY_CHUNK_SIZE)) {

    while (len-- > 0) {
      *out = *(out - dist);
      out++;
    }
    return out;
  }
  return chunkcopy_lapped_relaxed(out, dist, len);
}

static inline unsigned char FAR *chunkcopy_safe_ugly(unsigned char FAR *out,
                                                     unsigned dist,
                                                     unsigned len,
                                                     unsigned char FAR *limit) {
#if defined(__GNUC__) && !defined(__clang__)

  return chunkcopy_core_safe(out, out - dist, len, limit);
#elif defined(__clang__) && !defined(__aarch64__)

  return chunkcopy_core_safe(out, out - dist, len, limit);
#else

  return chunkcopy_lapped_safe(out, dist, len, limit);
#endif
}

#ifdef INFLATE_CHUNK_READ_64LE

typedef uint64_t inflate_holder_t;

static inline inflate_holder_t read64le(const unsigned char FAR *in) {
  inflate_holder_t input;
  Z_BUILTIN_MEMCPY(&input, in, sizeof(input));
  return input;
}
#else

typedef unsigned long inflate_holder_t;

#endif

#undef Z_STATIC_ASSERT
#undef Z_RESTRICT
#undef Z_BUILTIN_MEMCPY

#endif
