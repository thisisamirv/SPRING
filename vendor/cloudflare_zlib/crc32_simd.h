
/* crc32_simd.h
 *
 * Copyright 2017 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the Chromium source repository LICENSE file.
 */
#ifndef CRC32_SIMD_H
#define CRC32_SIMD_H

#include <stdint.h>

#include "zutil.h"

#ifdef HAS_PCLMUL
#define CRC32_SIMD_SSE42_PCLMUL
#endif

#ifndef z_size_t
#define z_size_t size_t
#endif
#ifndef zalign
#ifdef _MSC_VER
#define zalign(x) __declspec(align(x))
#else
#define zalign(x) __attribute__((aligned((x))))
#endif
#endif

uint32_t ZLIB_INTERNAL crc32_sse42_simd_(const unsigned char *buf, z_size_t len,
                                         uint32_t crc);

#define Z_CRC32_SSE42_MINIMUM_LENGTH 64
#define Z_CRC32_SSE42_CHUNKSIZE_MASK 15

uint32_t ZLIB_INTERNAL armv8_crc32_little(unsigned long crc,
                                          const unsigned char *buf,
                                          z_size_t len);

#endif