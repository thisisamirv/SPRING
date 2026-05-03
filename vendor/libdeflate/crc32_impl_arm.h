/*
 * arm/crc32_impl.h - ARM implementations of the gzip CRC-32 algorithm
 *
 * Copyright 2022 Eric Biggers
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

#ifndef LIB_ARM_CRC32_IMPL_H
#define LIB_ARM_CRC32_IMPL_H

#if defined(__arm__) || defined(__aarch64__)
#include "cpu_features_arm.h"
#include "crc32_multipliers.h"
#include "lib_common.h"

#ifndef CRC32_FUNC_T_DEFINED
#define CRC32_FUNC_T_DEFINED
typedef u32 (*crc32_func_t)(u32 crc, const u8 *p, size_t len);
#endif

#if defined(HAVE_CRC32_INTRIN) && HAVE_CRC32_INTRIN

#ifdef __clang__
#define ATTRIBUTES _target_attribute("crc")
#else
#define ATTRIBUTES _target_attribute("+crc")
#endif

static forceinline ATTRIBUTES u32 combine_crcs_slow(u32 crc0, u32 crc1,
                                                    u32 crc2, u32 crc3) {
  u64 res0 = 0, res1 = 0, res2 = 0;
  int i;

  for (i = 0; i < 32; i++) {
    if (CRC32_FIXED_CHUNK_MULT_3 & (1U << i))
      res0 ^= (u64)crc0 << i;
    if (CRC32_FIXED_CHUNK_MULT_2 & (1U << i))
      res1 ^= (u64)crc1 << i;
    if (CRC32_FIXED_CHUNK_MULT_1 & (1U << i))
      res2 ^= (u64)crc2 << i;
  }

  return __crc32d(0, res0 ^ res1 ^ res2) ^ crc3;
}

#define crc32_arm_crc crc32_arm_crc
static ATTRIBUTES u32 crc32_arm_crc(u32 crc, const u8 *p, size_t len) {
  if (len >= 64) {
    const size_t align = -(uintptr_t)p & 7;

    if (align) {
      if (align & 1)
        crc = __crc32b(crc, *p++);
      if (align & 2) {
        crc = __crc32h(crc, le16_bswap(*(u16 *)p));
        p += 2;
      }
      if (align & 4) {
        crc = __crc32w(crc, le32_bswap(*(u32 *)p));
        p += 4;
      }
      len -= align;
    }

    while (len >= CRC32_NUM_CHUNKS * CRC32_FIXED_CHUNK_LEN) {
      const u64 *wp0 = (const u64 *)p;
      const u64 *const wp0_end = (const u64 *)(p + CRC32_FIXED_CHUNK_LEN);
      u32 crc1 = 0, crc2 = 0, crc3 = 0;

      STATIC_ASSERT(CRC32_NUM_CHUNKS == 4);
      STATIC_ASSERT(CRC32_FIXED_CHUNK_LEN % (4 * 8) == 0);
      do {
        prefetchr(&wp0[64 + 0 * CRC32_FIXED_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 1 * CRC32_FIXED_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 2 * CRC32_FIXED_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 3 * CRC32_FIXED_CHUNK_LEN / 8]);
        crc = __crc32d(crc, le64_bswap(wp0[0 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1, le64_bswap(wp0[1 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2, le64_bswap(wp0[2 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3, le64_bswap(wp0[3 * CRC32_FIXED_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc, le64_bswap(wp0[0 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1, le64_bswap(wp0[1 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2, le64_bswap(wp0[2 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3, le64_bswap(wp0[3 * CRC32_FIXED_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc, le64_bswap(wp0[0 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1, le64_bswap(wp0[1 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2, le64_bswap(wp0[2 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3, le64_bswap(wp0[3 * CRC32_FIXED_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc, le64_bswap(wp0[0 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1, le64_bswap(wp0[1 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2, le64_bswap(wp0[2 * CRC32_FIXED_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3, le64_bswap(wp0[3 * CRC32_FIXED_CHUNK_LEN / 8]));
        wp0++;
      } while (wp0 != wp0_end);
      crc = combine_crcs_slow(crc, crc1, crc2, crc3);
      p += CRC32_NUM_CHUNKS * CRC32_FIXED_CHUNK_LEN;
      len -= CRC32_NUM_CHUNKS * CRC32_FIXED_CHUNK_LEN;
    }

    while (len >= 64) {
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 0)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 8)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 16)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 24)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 32)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 40)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 48)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 56)));
      p += 64;
      len -= 64;
    }
  }
  if (len & 32) {
    crc = __crc32d(crc, get_unaligned_le64(p + 0));
    crc = __crc32d(crc, get_unaligned_le64(p + 8));
    crc = __crc32d(crc, get_unaligned_le64(p + 16));
    crc = __crc32d(crc, get_unaligned_le64(p + 24));
    p += 32;
  }
  if (len & 16) {
    crc = __crc32d(crc, get_unaligned_le64(p + 0));
    crc = __crc32d(crc, get_unaligned_le64(p + 8));
    p += 16;
  }
  if (len & 8) {
    crc = __crc32d(crc, get_unaligned_le64(p));
    p += 8;
  }
  if (len & 4) {
    crc = __crc32w(crc, get_unaligned_le32(p));
    p += 4;
  }
  if (len & 2) {
    crc = __crc32h(crc, get_unaligned_le16(p));
    p += 2;
  }
  if (len & 1)
    crc = __crc32b(crc, *p);
  return crc;
}
#undef ATTRIBUTES
#endif

#if (defined(HAVE_CRC32_INTRIN) && HAVE_CRC32_INTRIN) &&                       \
    (defined(HAVE_PMULL_INTRIN) && HAVE_PMULL_INTRIN)

#ifdef __clang__
#define ATTRIBUTES _target_attribute("crc,aes")
#else
#define ATTRIBUTES _target_attribute("+crc,+crypto")
#endif

static forceinline ATTRIBUTES u64 clmul_u32(u32 a, u32 b) {
  uint64x2_t res =
      vreinterpretq_u64_p128(compat_vmull_p64((poly64_t)a, (poly64_t)b));

  return vgetq_lane_u64(res, 0);
}

static forceinline ATTRIBUTES u32 combine_crcs_fast(u32 crc0, u32 crc1,
                                                    u32 crc2, u32 crc3,
                                                    size_t i) {
  u64 res0 = clmul_u32(crc0, crc32_mults_for_chunklen[i][0]);
  u64 res1 = clmul_u32(crc1, crc32_mults_for_chunklen[i][1]);
  u64 res2 = clmul_u32(crc2, crc32_mults_for_chunklen[i][2]);

  return __crc32d(0, res0 ^ res1 ^ res2) ^ crc3;
}

#define crc32_arm_crc_pmullcombine crc32_arm_crc_pmullcombine
static ATTRIBUTES u32 crc32_arm_crc_pmullcombine(u32 crc, const u8 *p,
                                                 size_t len) {
  const size_t align = -(uintptr_t)p & 7;

  if (len >= align + CRC32_NUM_CHUNKS * CRC32_MIN_VARIABLE_CHUNK_LEN) {

    if (align) {
      if (align & 1)
        crc = __crc32b(crc, *p++);
      if (align & 2) {
        crc = __crc32h(crc, le16_bswap(*(u16 *)p));
        p += 2;
      }
      if (align & 4) {
        crc = __crc32w(crc, le32_bswap(*(u32 *)p));
        p += 4;
      }
      len -= align;
    }

    while (len >= CRC32_NUM_CHUNKS * CRC32_MAX_VARIABLE_CHUNK_LEN) {
      const u64 *wp0 = (const u64 *)p;
      const u64 *const wp0_end =
          (const u64 *)(p + CRC32_MAX_VARIABLE_CHUNK_LEN);
      u32 crc1 = 0, crc2 = 0, crc3 = 0;

      STATIC_ASSERT(CRC32_NUM_CHUNKS == 4);
      STATIC_ASSERT(CRC32_MAX_VARIABLE_CHUNK_LEN % (4 * 8) == 0);
      do {
        prefetchr(&wp0[64 + 0 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 1 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 2 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]);
        prefetchr(&wp0[64 + 3 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]);
        crc = __crc32d(crc,
                       le64_bswap(wp0[0 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1,
                        le64_bswap(wp0[1 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2,
                        le64_bswap(wp0[2 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3,
                        le64_bswap(wp0[3 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc,
                       le64_bswap(wp0[0 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1,
                        le64_bswap(wp0[1 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2,
                        le64_bswap(wp0[2 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3,
                        le64_bswap(wp0[3 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc,
                       le64_bswap(wp0[0 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1,
                        le64_bswap(wp0[1 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2,
                        le64_bswap(wp0[2 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3,
                        le64_bswap(wp0[3 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        wp0++;
        crc = __crc32d(crc,
                       le64_bswap(wp0[0 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc1 = __crc32d(crc1,
                        le64_bswap(wp0[1 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc2 = __crc32d(crc2,
                        le64_bswap(wp0[2 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        crc3 = __crc32d(crc3,
                        le64_bswap(wp0[3 * CRC32_MAX_VARIABLE_CHUNK_LEN / 8]));
        wp0++;
      } while (wp0 != wp0_end);
      crc = combine_crcs_fast(crc, crc1, crc2, crc3,
                              ARRAY_LEN(crc32_mults_for_chunklen) - 1);
      p += CRC32_NUM_CHUNKS * CRC32_MAX_VARIABLE_CHUNK_LEN;
      len -= CRC32_NUM_CHUNKS * CRC32_MAX_VARIABLE_CHUNK_LEN;
    }

    if (len >= CRC32_NUM_CHUNKS * CRC32_MIN_VARIABLE_CHUNK_LEN) {
      const size_t i = len / (CRC32_NUM_CHUNKS * CRC32_MIN_VARIABLE_CHUNK_LEN);
      const size_t chunk_len = i * CRC32_MIN_VARIABLE_CHUNK_LEN;
      const u64 *wp0 = (const u64 *)(p + 0 * chunk_len);
      const u64 *wp1 = (const u64 *)(p + 1 * chunk_len);
      const u64 *wp2 = (const u64 *)(p + 2 * chunk_len);
      const u64 *wp3 = (const u64 *)(p + 3 * chunk_len);
      const u64 *const wp0_end = wp1;
      u32 crc1 = 0, crc2 = 0, crc3 = 0;

      STATIC_ASSERT(CRC32_NUM_CHUNKS == 4);
      STATIC_ASSERT(CRC32_MIN_VARIABLE_CHUNK_LEN % (4 * 8) == 0);
      do {
        prefetchr(wp0 + 64);
        prefetchr(wp1 + 64);
        prefetchr(wp2 + 64);
        prefetchr(wp3 + 64);
        crc = __crc32d(crc, le64_bswap(*wp0++));
        crc1 = __crc32d(crc1, le64_bswap(*wp1++));
        crc2 = __crc32d(crc2, le64_bswap(*wp2++));
        crc3 = __crc32d(crc3, le64_bswap(*wp3++));
        crc = __crc32d(crc, le64_bswap(*wp0++));
        crc1 = __crc32d(crc1, le64_bswap(*wp1++));
        crc2 = __crc32d(crc2, le64_bswap(*wp2++));
        crc3 = __crc32d(crc3, le64_bswap(*wp3++));
        crc = __crc32d(crc, le64_bswap(*wp0++));
        crc1 = __crc32d(crc1, le64_bswap(*wp1++));
        crc2 = __crc32d(crc2, le64_bswap(*wp2++));
        crc3 = __crc32d(crc3, le64_bswap(*wp3++));
        crc = __crc32d(crc, le64_bswap(*wp0++));
        crc1 = __crc32d(crc1, le64_bswap(*wp1++));
        crc2 = __crc32d(crc2, le64_bswap(*wp2++));
        crc3 = __crc32d(crc3, le64_bswap(*wp3++));
      } while (wp0 != wp0_end);
      crc = combine_crcs_fast(crc, crc1, crc2, crc3, i);
      p += CRC32_NUM_CHUNKS * chunk_len;
      len -= CRC32_NUM_CHUNKS * chunk_len;
    }

    while (len >= 32) {
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 0)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 8)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 16)));
      crc = __crc32d(crc, le64_bswap(*(u64 *)(p + 24)));
      p += 32;
      len -= 32;
    }
  } else {
    while (len >= 32) {
      crc = __crc32d(crc, get_unaligned_le64(p + 0));
      crc = __crc32d(crc, get_unaligned_le64(p + 8));
      crc = __crc32d(crc, get_unaligned_le64(p + 16));
      crc = __crc32d(crc, get_unaligned_le64(p + 24));
      p += 32;
      len -= 32;
    }
  }
  if (len & 16) {
    crc = __crc32d(crc, get_unaligned_le64(p + 0));
    crc = __crc32d(crc, get_unaligned_le64(p + 8));
    p += 16;
  }
  if (len & 8) {
    crc = __crc32d(crc, get_unaligned_le64(p));
    p += 8;
  }
  if (len & 4) {
    crc = __crc32w(crc, get_unaligned_le32(p));
    p += 4;
  }
  if (len & 2) {
    crc = __crc32h(crc, get_unaligned_le16(p));
    p += 2;
  }
  if (len & 1)
    crc = __crc32b(crc, *p);
  return crc;
}
#undef ATTRIBUTES
#endif

#if defined(HAVE_PMULL_INTRIN) && HAVE_PMULL_INTRIN

#ifndef CONCAT_IMPL
#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)
#define ADD_SUFFIX(name) CONCAT(name, SUFFIX)
#endif
#define crc32_arm_pmullx4 crc32_arm_pmullx4
#define SUFFIX _pmullx4
#ifdef __clang__

#define ATTRIBUTES _target_attribute("aes")
#else

#define ATTRIBUTES _target_attribute("+crypto")
#endif
#define ENABLE_EOR3 0
#include "crc32_pmull_helpers.h"

#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)
#define ADD_SUFFIX(name) CONCAT(name, SUFFIX)
#define u32_to_bytevec ADD_SUFFIX(u32_to_bytevec)
#define load_multipliers ADD_SUFFIX(load_multipliers)
#define clmul_low ADD_SUFFIX(clmul_low)
#define fold_vec ADD_SUFFIX(fold_vec)
#define fold_partial_vec ADD_SUFFIX(fold_partial_vec)

static ATTRIBUTES u32 crc32_arm_pmullx4(u32 crc, const u8 *p, size_t len) {
  static const u64 _aligned_attribute(16) mults[3][2] = {
      {CRC32_X159_MODG, CRC32_X95_MODG},
      {CRC32_X543_MODG, CRC32_X479_MODG},
      {CRC32_X287_MODG, CRC32_X223_MODG},
  };
  static const u64 _aligned_attribute(16) barrett_consts[3][2] = {
      {
          CRC32_X95_MODG,
      },
      {
          CRC32_BARRETT_CONSTANT_1,
      },
      {
          CRC32_BARRETT_CONSTANT_2,
      },
  };
  const poly64x2_t multipliers_1 = load_multipliers(mults[0]);
  uint8x16_t v0, v1, v2, v3;

  if (len < 64 + 15) {
    if (len < 16)
      return crc32_slice1(crc, p, len);
    v0 = veorq_u8(vld1q_u8(p), u32_to_bytevec(crc));
    p += 16;
    len -= 16;
    while (len >= 16) {
      v0 = fold_vec(v0, vld1q_u8(p), multipliers_1);
      p += 16;
      len -= 16;
    }
  } else {
    const poly64x2_t multipliers_4 = load_multipliers(mults[1]);
    const poly64x2_t multipliers_2 = load_multipliers(mults[2]);
    const size_t align = -(uintptr_t)p & 15;
    const uint8x16_t *vp;

    v0 = veorq_u8(vld1q_u8(p), u32_to_bytevec(crc));
    p += 16;

    if (align) {
      v0 = fold_partial_vec(v0, p, align, multipliers_1);
      p += align;
      len -= align;
    }
    vp = (const uint8x16_t *)p;
    v1 = *vp++;
    v2 = *vp++;
    v3 = *vp++;
    while (len >= 64 + 64) {
      v0 = fold_vec(v0, *vp++, multipliers_4);
      v1 = fold_vec(v1, *vp++, multipliers_4);
      v2 = fold_vec(v2, *vp++, multipliers_4);
      v3 = fold_vec(v3, *vp++, multipliers_4);
      len -= 64;
    }
    v0 = fold_vec(v0, v2, multipliers_2);
    v1 = fold_vec(v1, v3, multipliers_2);
    if (len & 32) {
      v0 = fold_vec(v0, *vp++, multipliers_2);
      v1 = fold_vec(v1, *vp++, multipliers_2);
    }
    v0 = fold_vec(v0, v1, multipliers_1);
    if (len & 16)
      v0 = fold_vec(v0, *vp++, multipliers_1);
    p = (const u8 *)vp;
    len &= 15;
  }

  if (len)
    v0 = fold_partial_vec(v0, p, len, multipliers_1);

  v0 = veorq_u8(clmul_low(v0, load_multipliers(barrett_consts[0])),
                vextq_u8(v0, vdupq_n_u8(0), 8));
  v1 = clmul_low(v0, load_multipliers(barrett_consts[1]));
  v1 = clmul_low(v1, load_multipliers(barrett_consts[2]));
  v0 = veorq_u8(v0, v1);
  return vgetq_lane_u32(vreinterpretq_u32_u8(v0), 2);
}
#undef SUFFIX
#undef ATTRIBUTES
#undef ENABLE_EOR3
#endif

#if (defined(HAVE_PMULL_INTRIN) && HAVE_PMULL_INTRIN) &&                       \
    (defined(HAVE_CRC32_INTRIN) && HAVE_CRC32_INTRIN)

#define crc32_arm_pmullx12_crc crc32_arm_pmullx12_crc
#define SUFFIX _pmullx12_crc
#ifdef __clang__
#define ATTRIBUTES _target_attribute("aes,crc")
#else
#define ATTRIBUTES _target_attribute("+crypto,+crc")
#endif
#define ENABLE_EOR3 0
#include "crc32_pmull_wide.h"
#endif

#if (defined(HAVE_PMULL_INTRIN) && HAVE_PMULL_INTRIN) &&                       \
    (defined(HAVE_CRC32_INTRIN) && HAVE_CRC32_INTRIN) &&                       \
    (defined(HAVE_SHA3_INTRIN) && HAVE_SHA3_INTRIN) &&                         \
    !defined(LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_SHA3)

#define crc32_arm_pmullx12_crc_eor3 crc32_arm_pmullx12_crc_eor3
#define SUFFIX _pmullx12_crc_eor3
#ifdef __clang__
#define ATTRIBUTES _target_attribute("aes,crc,sha3")

#elif GCC_PREREQ(14, 0) || defined(__ARM_FEATURE_JCVT) ||                      \
    defined(__ARM_FEATURE_DOTPROD)
#define ATTRIBUTES _target_attribute("+crypto,+crc,+sha3")
#else
#define ATTRIBUTES _target_attribute("arch=armv8.2-a+crypto+crc+sha3")
#endif
#define ENABLE_EOR3 1
#include "crc32_pmull_wide.h"
#endif

static inline crc32_func_t arch_select_crc32_func(void) {
  const u32 features MAYBE_UNUSED = get_arm_cpu_features();

#ifdef crc32_arm_pmullx12_crc_eor3
  if ((features & ARM_CPU_FEATURE_PREFER_PMULL) && HAVE_PMULL(features) &&
      HAVE_CRC32(features) && HAVE_SHA3(features))
    return crc32_arm_pmullx12_crc_eor3;
#endif
#ifdef crc32_arm_pmullx12_crc
  if ((features & ARM_CPU_FEATURE_PREFER_PMULL) && HAVE_PMULL(features) &&
      HAVE_CRC32(features))
    return crc32_arm_pmullx12_crc;
#endif
#ifdef crc32_arm_crc_pmullcombine
  if (HAVE_CRC32(features) && HAVE_PMULL(features))
    return crc32_arm_crc_pmullcombine;
#endif
#ifdef crc32_arm_crc
  if (HAVE_CRC32(features))
    return crc32_arm_crc;
#endif
#ifdef crc32_arm_pmullx4
  if (HAVE_PMULL(features))
    return crc32_arm_pmullx4;
#endif
  return NULL;
}
#define arch_select_crc32_func arch_select_crc32_func

#endif

#endif
