/*
 * adler32.c - Adler-32 checksum algorithm
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

#include "libdeflate.h"

#define DIVISOR 65521

#define MAX_CHUNK_LEN 5552

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

static u32 MAYBE_UNUSED adler32_generic(u32 adler, const u8 *p, size_t len) {
  u32 s1 = adler & 0xFFFF;
  u32 s2 = adler >> 16;

  while (len) {
    size_t n = MIN(len, MAX_CHUNK_LEN & ~3);

    len -= n;
    ADLER32_CHUNK(s1, s2, p, n);
  }

  return (s2 << 16) | s1;
}

#undef DEFAULT_IMPL
#undef arch_select_adler32_func
#ifndef ADLER32_FUNC_T_DEFINED
#define ADLER32_FUNC_T_DEFINED
typedef u32 (*adler32_func_t)(u32 adler, const u8 *p, size_t len);
#endif

#include "adler32_impl_x86.h"
#if defined(ARCH_ARM32) || defined(ARCH_ARM64)
#include "adler32_impl_arm.h"
#endif

#ifndef DEFAULT_IMPL
#define DEFAULT_IMPL adler32_generic
#endif

#if ADLER32_HAS_X86_IMPL
static u32 dispatch_adler32(u32 adler, const u8 *p, size_t len);

static volatile adler32_func_t adler32_impl = dispatch_adler32;

static u32 dispatch_adler32(u32 adler, const u8 *p, size_t len) {
  adler32_func_t f = arch_select_adler32_func();

  if (f == NULL)
    f = DEFAULT_IMPL;

  adler32_impl = f;
  return f(adler, p, len);
}
#else

#define adler32_impl DEFAULT_IMPL
#endif

LIBDEFLATEAPI u32 libdeflate_adler32(u32 adler, const void *buffer,
                                     size_t len) {
  if (buffer == NULL)
    return 1;
  return adler32_impl(adler, buffer, len);
}
