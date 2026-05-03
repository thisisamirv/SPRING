/*
 * x86/crc32_impl.h - x86 implementations of the gzip CRC-32 algorithm
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

#ifndef LIB_X86_CRC32_IMPL_H
#define LIB_X86_CRC32_IMPL_H

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include "common_defs.h"
#include "cpu_features_x86.h"
#include <immintrin.h>

#ifndef crc32_slice1
extern u32 crc32_slice1(u32 crc, const u8 *p, size_t len);
#endif
#ifndef CRC32_FUNC_T_DEFINED
#define CRC32_FUNC_T_DEFINED
typedef u32 (*crc32_func_t)(u32 crc, const u8 *p, size_t len);
#endif

static const u8 MAYBE_UNUSED shift_tab[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)

#define crc32_x86_pclmulqdq crc32_x86_pclmulqdq
#define SUFFIX _pclmulqdq
#define ATTRIBUTES _target_attribute("pclmul,sse4.1")
#define VL 16
#define USE_AVX512 0
#include "crc32_pclmul_template.h"

#define crc32_x86_pclmulqdq_avx crc32_x86_pclmulqdq_avx
#define SUFFIX _pclmulqdq_avx
#define ATTRIBUTES _target_attribute("pclmul,avx")
#define VL 16
#define USE_AVX512 0
#include "crc32_pclmul_template.h"
#endif

#if (GCC_PREREQ(10, 1) || CLANG_PREREQ(6, 0, 10000000)) &&                     \
    !defined(LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_VPCLMULQDQ)
#define crc32_x86_vpclmulqdq_avx2 crc32_x86_vpclmulqdq_avx2
#define SUFFIX _vpclmulqdq_avx2
#define ATTRIBUTES _target_attribute("vpclmulqdq,pclmul,avx2")
#define VL 32
#define USE_AVX512 0
#include "crc32_pclmul_template.h"
#endif

#if (GCC_PREREQ(10, 1) || CLANG_PREREQ(6, 0, 10000000) ||                      \
     MSVC_PREREQ(1920)) &&                                                     \
    !defined(LIBDEFLATE_ASSEMBLER_DOES_NOT_SUPPORT_VPCLMULQDQ)

#define crc32_x86_vpclmulqdq_avx512_vl256 crc32_x86_vpclmulqdq_avx512_vl256
#define SUFFIX _vpclmulqdq_avx512_vl256
#define ATTRIBUTES _target_attribute("vpclmulqdq,pclmul,avx512bw,avx512vl")
#define VL 32
#define USE_AVX512 1
#include "crc32_pclmul_template.h"

#define crc32_x86_vpclmulqdq_avx512_vl512 crc32_x86_vpclmulqdq_avx512_vl512
#define SUFFIX _vpclmulqdq_avx512_vl512
#define ATTRIBUTES _target_attribute("vpclmulqdq,pclmul,avx512bw,avx512vl")
#define VL 64
#define USE_AVX512 1
#include "crc32_pclmul_template.h"
#endif

static inline crc32_func_t arch_select_crc32_func(void) {
  const u32 features MAYBE_UNUSED = get_x86_cpu_features();

#ifdef crc32_x86_vpclmulqdq_avx512_vl512
  if ((features & X86_CPU_FEATURE_ZMM) && HAVE_VPCLMULQDQ(features) &&
      HAVE_PCLMULQDQ(features) && HAVE_AVX512BW(features) &&
      HAVE_AVX512VL(features))
    return crc32_x86_vpclmulqdq_avx512_vl512;
#endif
#ifdef crc32_x86_vpclmulqdq_avx512_vl256
  if (HAVE_VPCLMULQDQ(features) && HAVE_PCLMULQDQ(features) &&
      HAVE_AVX512BW(features) && HAVE_AVX512VL(features))
    return crc32_x86_vpclmulqdq_avx512_vl256;
#endif
#ifdef crc32_x86_vpclmulqdq_avx2
  if (HAVE_VPCLMULQDQ(features) && HAVE_PCLMULQDQ(features) &&
      HAVE_AVX2(features))
    return crc32_x86_vpclmulqdq_avx2;
#endif
#ifdef crc32_x86_pclmulqdq_avx
  if (HAVE_PCLMULQDQ(features) && HAVE_AVX(features))
    return crc32_x86_pclmulqdq_avx;
#endif
#ifdef crc32_x86_pclmulqdq
  if (HAVE_PCLMULQDQ(features))
    return crc32_x86_pclmulqdq;
#endif
  return NULL;
}
#define arch_select_crc32_func arch_select_crc32_func

#endif

#endif
