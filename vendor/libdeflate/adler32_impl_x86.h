/*
 * x86/adler32_impl.h - x86 implementations of Adler-32 checksum algorithm
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

#ifndef LIB_X86_ADLER32_IMPL_H
#define LIB_X86_ADLER32_IMPL_H

#ifndef ADLER32_HAS_X86_IMPL
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#define ADLER32_HAS_X86_IMPL 1
#else
#define ADLER32_HAS_X86_IMPL 0
#endif
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include "common_defs.h"

#ifndef DIVISOR
#define DIVISOR 65521
#define MAX_CHUNK_LEN 5552
#endif

#ifndef ADLER32_CHUNK
#define ADLER32_CHUNK(s1, s2, p, n)                                            \
  do {                                                                         \
    if (n >= 4) {                                                              \
      u32 s1_sum = 0;                                                          \
      u32 byte_0_sum = 0;                                                      \
      u32 byte_1_sum = 0;                                                      \
      u32 byte_2_sum = 0;                                                      \
      u32 byte_3_sum = 0;                                                      \
                                                                               \
      do {                                                                     \
        s1_sum += s1;                                                          \
        s1 += p[0] + p[1] + p[2] + p[3];                                       \
        byte_0_sum += p[0];                                                    \
        byte_1_sum += p[1];                                                    \
        byte_2_sum += p[2];                                                    \
        byte_3_sum += p[3];                                                    \
        p += 4;                                                                \
        n -= 4;                                                                \
      } while (n >= 4);                                                        \
      s2 += (4 * (s1_sum + byte_0_sum)) + (3 * byte_1_sum) +                   \
            (2 * byte_2_sum) + byte_3_sum;                                     \
    }                                                                          \
    for (; n; n--, p++) {                                                      \
      s1 += *p;                                                                \
      s2 += s1;                                                                \
    }                                                                          \
    s1 %= DIVISOR;                                                             \
    s2 %= DIVISOR;                                                             \
  } while (0)
#endif

#include "cpu_features_x86.h"

#ifndef ADLER32_FUNC_T_DEFINED
#define ADLER32_FUNC_T_DEFINED
typedef u32 (*adler32_func_t)(u32 adler, const u8 *p, size_t len);
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#define adler32_x86_sse2 adler32_x86_sse2
#define SUFFIX _sse2
#define ATTRIBUTES _target_attribute("sse2")
#define VL 16
#define USE_VNNI 0
#define USE_AVX512 0
#include "adler32_template.h"

#define adler32_x86_avx2 adler32_x86_avx2
#define SUFFIX _avx2
#define ATTRIBUTES _target_attribute("avx2")
#define VL 32
#define USE_VNNI 0
#define USE_AVX512 0
#include "adler32_template.h"
#endif

#if (GCC_PREREQ(12, 1) || CLANG_PREREQ(12, 0, 13000000) ||                     \
     MSVC_PREREQ(1930)) &&                                                     \
    !defined(LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_AVX_VNNI)
#define adler32_x86_avx2_vnni adler32_x86_avx2_vnni
#define SUFFIX _avx2_vnni
#define ATTRIBUTES _target_attribute("avx2,avxvnni")
#define VL 32
#define USE_VNNI 1
#define USE_AVX512 0
#include "adler32_template.h"
#endif

#if (GCC_PREREQ(8, 1) || CLANG_PREREQ(6, 0, 10000000) || MSVC_PREREQ(1920)) && \
    !defined(LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_AVX512VNNI)

#define adler32_x86_avx512_vl256_vnni adler32_x86_avx512_vl256_vnni
#define SUFFIX _avx512_vl256_vnni
#define ATTRIBUTES _target_attribute("avx512bw,avx512vl,avx512vnni")
#define VL 32
#define USE_VNNI 1
#define USE_AVX512 1
#include "adler32_template.h"

#define adler32_x86_avx512_vl512_vnni adler32_x86_avx512_vl512_vnni
#define SUFFIX _avx512_vl512_vnni
#define ATTRIBUTES _target_attribute("avx512bw,avx512vnni")
#define VL 64
#define USE_VNNI 1
#define USE_AVX512 1
#include "adler32_template.h"
#endif

static inline adler32_func_t arch_select_adler32_func(void) {
  const u32 features MAYBE_UNUSED = get_x86_cpu_features();

#ifdef adler32_x86_avx512_vl512_vnni
  if ((features & X86_CPU_FEATURE_ZMM) && HAVE_AVX512BW(features) &&
      HAVE_AVX512VNNI(features))
    return adler32_x86_avx512_vl512_vnni;
#endif
#ifdef adler32_x86_avx512_vl256_vnni
  if (HAVE_AVX512BW(features) && HAVE_AVX512VL(features) &&
      HAVE_AVX512VNNI(features))
    return adler32_x86_avx512_vl256_vnni;
#endif
#ifdef adler32_x86_avx2_vnni
  if (HAVE_AVX2(features) && HAVE_AVXVNNI(features))
    return adler32_x86_avx2_vnni;
#endif
#ifdef adler32_x86_avx2
  if (HAVE_AVX2(features))
    return adler32_x86_avx2;
#endif
#ifdef adler32_x86_sse2
  if (HAVE_SSE2(features))
    return adler32_x86_sse2;
#endif
  return NULL;
}
#define arch_select_adler32_func arch_select_adler32_func

#endif

#endif
