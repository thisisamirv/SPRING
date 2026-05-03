/*
 * crc32.c - CRC-32 checksum algorithm for the gzip format
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

#include "crc32_tables.h"
#include "libdeflate.h"

static u32 MAYBE_UNUSED crc32_slice8(u32 crc, const u8 *p, size_t len) {
  const u8 *const end = p + len;
  const u8 *end64;

  for (; ((uintptr_t)p & 7) && p != end; p++)
    crc = (crc >> 8) ^ crc32_slice8_table[(u8)crc ^ *p];

  end64 = p + ((end - p) & ~7);
  for (; p != end64; p += 8) {
    u32 v1 = le32_bswap(*(const u32 *)(p + 0));
    u32 v2 = le32_bswap(*(const u32 *)(p + 4));

    crc = crc32_slice8_table[0x700 + (u8)((crc ^ v1) >> 0)] ^
          crc32_slice8_table[0x600 + (u8)((crc ^ v1) >> 8)] ^
          crc32_slice8_table[0x500 + (u8)((crc ^ v1) >> 16)] ^
          crc32_slice8_table[0x400 + (u8)((crc ^ v1) >> 24)] ^
          crc32_slice8_table[0x300 + (u8)(v2 >> 0)] ^
          crc32_slice8_table[0x200 + (u8)(v2 >> 8)] ^
          crc32_slice8_table[0x100 + (u8)(v2 >> 16)] ^
          crc32_slice8_table[0x000 + (u8)(v2 >> 24)];
  }

  for (; p != end; p++)
    crc = (crc >> 8) ^ crc32_slice8_table[(u8)crc ^ *p];

  return crc;
}

static forceinline u32 MAYBE_UNUSED crc32_slice1(u32 crc, const u8 *p,
                                                 size_t len) {
  size_t i;

  for (i = 0; i < len; i++)
    crc = (crc >> 8) ^ crc32_slice1_table[(u8)crc ^ p[i]];
  return crc;
}

#undef DEFAULT_IMPL
#undef arch_select_crc32_func
#ifndef CRC32_FUNC_T_DEFINED
#define CRC32_FUNC_T_DEFINED
typedef u32 (*crc32_func_t)(u32 crc, const u8 *p, size_t len);
#endif
#if defined(ARCH_ARM32) || defined(ARCH_ARM64)
#include "crc32_impl_arm.h"
#elif defined(ARCH_X86_32) || defined(ARCH_X86_64)
#include "crc32_impl_x86.h"
#endif

#ifndef DEFAULT_IMPL
#define DEFAULT_IMPL crc32_slice8
#endif

#ifdef arch_select_crc32_func
static u32 dispatch_crc32(u32 crc, const u8 *p, size_t len);

static volatile crc32_func_t crc32_impl = dispatch_crc32;

static u32 dispatch_crc32(u32 crc, const u8 *p, size_t len) {
  crc32_func_t f = arch_select_crc32_func();

  if (f == NULL)
    f = DEFAULT_IMPL;

  crc32_impl = f;
  return f(crc, p, len);
}
#else

#define crc32_impl DEFAULT_IMPL
#endif

LIBDEFLATEAPI u32 libdeflate_crc32(u32 crc, const void *p, size_t len) {
  if (p == NULL)
    return 0;
  return ~crc32_impl(~crc, p, len);
}
