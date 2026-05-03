/* crc32.c -- compute the CRC-32 of a data stream
 * Copyright (C) 1995-2022 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * This interleaved implementation of a CRC makes use of pipelined multiple
 * arithmetic-logic units, commonly found in modern CPU cores. It is due to
 * Kadatch and Jenkins (2010). See doc/crc-doc.1.0.pdf in this distribution.
 */

#ifdef HAS_PCLMUL
#include "crc32_simd.h"
#ifndef _MSC_VER
#include <cpuid.h>
#endif
#endif

#ifdef __aarch64__

#include <arm_acle.h>
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

uint32_t crc32(uint32_t crc, uint8_t *buf, size_t len) {
  crc = ~crc;

  while (len >= 8) {
    crc = __crc32d(crc, *(uint64_t *)buf);
    len -= 8;
    buf += 8;
  }

  if (len & 4) {
    crc = __crc32w(crc, *(uint32_t *)buf);
    buf += 4;
  }
  if (len & 2) {
    crc = __crc32h(crc, *(uint16_t *)buf);
    buf += 2;
  }
  if (len & 1) {
    crc = __crc32b(crc, *buf);
  }

  return ~crc;
}

#else

#ifdef MAKECRCH
#include <stdio.h>
#ifndef DYNAMIC_CRC_TABLE
#define DYNAMIC_CRC_TABLE
#endif
#endif

#include "zutil.h"

#define local static

#if !defined(NOBYFOUR) && defined(Z_U4)
#define BYFOUR
#endif
#ifdef BYFOUR
local unsigned long crc32_little(unsigned long, const unsigned char FAR *,
                                 unsigned);
local unsigned long crc32_big(unsigned long, const unsigned char FAR *,
                              unsigned);
#define TBLS 8
#else
#define TBLS 1
#endif

local unsigned long gf2_matrix_times(unsigned long *mat, unsigned long vec);
local void gf2_matrix_square(unsigned long *square, unsigned long *mat);
local uLong crc32_combine_(uLong crc1, uLong crc2, z_off64_t len2);

#ifdef DYNAMIC_CRC_TABLE

local volatile int crc_table_empty = 1;
local z_crc_t FAR crc_table[TBLS][256];
local void make_crc_table(void);
#ifdef MAKECRCH
local void write_table(FILE *, const z_crc_t FAR *);
#endif

local void make_crc_table() {
  z_crc_t c;
  int n, k;
  z_crc_t poly;

  static volatile int first = 1;
  static const unsigned char p[] = {0,  1,  2,  4,  5,  7,  8,
                                    10, 11, 12, 16, 22, 23, 26};

  if (first) {
    first = 0;

    poly = 0;
    for (n = 0; n < (int)(sizeof(p) / sizeof(unsigned char)); n++)
      poly |= (z_crc_t)1 << (31 - p[n]);

    for (n = 0; n < 256; n++) {
      c = (z_crc_t)n;
      for (k = 0; k < 8; k++)
        c = c & 1 ? poly ^ (c >> 1) : c >> 1;
      crc_table[0][n] = c;
    }

#ifdef BYFOUR

    for (n = 0; n < 256; n++) {
      c = crc_table[0][n];
      crc_table[4][n] = ZSWAP32(c);
      for (k = 1; k < 4; k++) {
        c = crc_table[0][c & 0xff] ^ (c >> 8);
        crc_table[k][n] = c;
        crc_table[k + 4][n] = ZSWAP32(c);
      }
    }
#endif

    crc_table_empty = 0;
  } else {

    while (crc_table_empty)
      ;
  }

#ifdef MAKECRCH

  {
    FILE *out;

    out = fopen("crc32.h", "w");
    if (out == NULL)
      return;
    fprintf(out, "/* crc32.h -- tables for rapid CRC calculation\n");
    fprintf(out, " * Generated automatically by crc32.c\n */\n\n");
    fprintf(out, "local const z_crc_t FAR ");
    fprintf(out, "crc_table[TBLS][256] =\n{\n  {\n");
    write_table(out, crc_table[0]);
#ifdef BYFOUR
    fprintf(out, "#ifdef BYFOUR\n");
    for (k = 1; k < 8; k++) {
      fprintf(out, "  },\n  {\n");
      write_table(out, crc_table[k]);
    }
    fprintf(out, "#endif\n");
#endif
    fprintf(out, "  }\n};\n");
    fclose(out);
  }
#endif
}

#ifdef MAKECRCH
static void write_table(FILE *out, const z_crc_t FAR *table) {
  int n;

  for (n = 0; n < 256; n++)
    fprintf(out, "%s0x%08lxUL%s", n % 5 ? "" : "    ",
            (unsigned long)(table[n]),
            n == 255 ? "\n" : (n % 5 == 4 ? ",\n" : ", "));
}
#endif

#else

#include "crc32.h"
#endif

const z_crc_t FAR *ZEXPORT get_crc_table() {
#ifdef DYNAMIC_CRC_TABLE
  if (crc_table_empty)
    make_crc_table();
#endif
  return (const z_crc_t FAR *)crc_table;
}

#define DO1 crc = crc_table[0][((int)crc ^ (*buf++)) & 0xff] ^ (crc >> 8)
#define DO8                                                                    \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1;                                                                         \
  DO1

static unsigned long crc32_generic(unsigned long crc,
                                   const unsigned char FAR *buf, uInt len) {
  if (buf == Z_NULL)
    return 0UL;

#ifdef DYNAMIC_CRC_TABLE
  if (crc_table_empty)
    make_crc_table();
#endif

#ifdef BYFOUR
  if (sizeof(void *) == sizeof(z_size_t)) {
    z_crc_t endian;

    endian = 1;
    if (*((unsigned char *)(&endian)))
      return crc32_little(crc, buf, len);
    else
      return crc32_big(crc, buf, len);
  }
#endif
  crc = crc ^ 0xffffffffUL;
  while (len >= 8) {
    DO8;
    len -= 8;
  }
  if (len)
    do {
      DO1;
    } while (--len);
  return crc ^ 0xffffffffUL;
}

#ifdef HAS_PCLMUL

#define PCLMUL_MIN_LEN 64
#define PCLMUL_ALIGN 16
#define PCLMUL_ALIGN_MASK 15

#if defined(__GNUC__)
#if __GNUC__ < 5
int cpu_has_pclmul = -1;

#else
_Atomic int cpu_has_pclmul = -1;
#endif
#else
#ifdef _MSC_VER
int cpu_has_pclmul = -1;

#else
_Atomic int cpu_has_pclmul = -1;
#endif
#endif

int has_pclmul(void) {
  if (cpu_has_pclmul >= 0)
    return cpu_has_pclmul;
  cpu_has_pclmul = 0;
  int leaf = 1;
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

#define crc_bit_PCLMUL (1 << 1)
#ifdef _MSC_VER
  uint32_t regs[4];
  __cpuid(regs, leaf);
  if (leaf == 1) {
    ecx = regs[2];
#else
  if (__get_cpuid(leaf, &eax, &ebx, &ecx, &edx)) {
#endif
    if ((ecx & crc_bit_PCLMUL) != 0)
      cpu_has_pclmul = 1;
  }
  return cpu_has_pclmul;
}

uLong crc32(uLong crc, const Bytef *buf, uInt len) {
  if (len < PCLMUL_MIN_LEN + PCLMUL_ALIGN - 1)
    return crc32_generic(crc, buf, len);
#ifndef SKIP_CPUID_CHECK
  if (!has_pclmul())
    return crc32_generic(crc, buf, len);
#endif

  uInt misalign = PCLMUL_ALIGN_MASK & ((unsigned long)buf);
  uInt sz = (PCLMUL_ALIGN - misalign) % PCLMUL_ALIGN;
  if (sz) {
    crc = crc32_generic(crc, buf, sz);
    buf += sz;
    len -= sz;
  }

  crc = crc32_sse42_simd_(buf, (len & ~PCLMUL_ALIGN_MASK), crc ^ 0xffffffffUL);
  crc = crc ^ 0xffffffffUL;

  sz = len & PCLMUL_ALIGN_MASK;
  if (sz) {
    crc = crc32_generic(crc, buf + len - sz, sz);
  }

  return crc;
}
#undef PCLMUL_MIN_LEN
#undef PCLMUL_ALIGN
#undef PCLMUL_ALIGN_MASK

#else
uLong crc32(uLong crc, const Bytef *buf, uInt len) {
  return crc32_generic(crc, buf, len);
}
#endif

#ifdef BYFOUR

#define DOLIT4                                                                 \
  c ^= *buf4++;                                                                \
  c = crc_table[3][c & 0xff] ^ crc_table[2][(c >> 8) & 0xff] ^                 \
      crc_table[1][(c >> 16) & 0xff] ^ crc_table[0][c >> 24]
#define DOLIT32                                                                \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4;                                                                      \
  DOLIT4

static unsigned long crc32_little(unsigned long crc,
                                  const unsigned char FAR *buf, unsigned len) {
  z_crc_t c;
  const z_crc_t FAR *buf4;

  c = (z_crc_t)crc;
  c = ~c;
  while (len && ((z_size_t)buf & 3)) {
    c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
    len--;
  }

  buf4 = (const z_crc_t FAR *)(const void FAR *)buf;
  while (len >= 32) {
    DOLIT32;
    len -= 32;
  }
  while (len >= 4) {
    DOLIT4;
    len -= 4;
  }
  buf = (const unsigned char FAR *)buf4;

  if (len)
    do {
      c = crc_table[0][(c ^ *buf++) & 0xff] ^ (c >> 8);
    } while (--len);
  c = ~c;
  return (unsigned long)c;
}

#define DOBIG4                                                                 \
  c ^= *buf4++;                                                                \
  c = crc_table[4][c & 0xff] ^ crc_table[5][(c >> 8) & 0xff] ^                 \
      crc_table[6][(c >> 16) & 0xff] ^ crc_table[7][c >> 24]
#define DOBIG32                                                                \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4;                                                                      \
  DOBIG4

static unsigned long crc32_big(unsigned long crc, const unsigned char FAR *buf,
                               unsigned len) {
  z_crc_t c;
  const z_crc_t FAR *buf4;

  c = ZSWAP32((z_crc_t)crc);
  c = ~c;
  while (len && ((z_size_t)buf & 3)) {
    c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
    len--;
  }

  buf4 = (const z_crc_t FAR *)(const void FAR *)buf;
  while (len >= 32) {
    DOBIG32;
    len -= 32;
  }
  while (len >= 4) {
    DOBIG4;
    len -= 4;
  }
  buf = (const unsigned char FAR *)buf4;

  if (len)
    do {
      c = crc_table[4][(c >> 24) ^ *buf++] ^ (c << 8);
    } while (--len);
  c = ~c;
  return (unsigned long)(ZSWAP32(c));
}

#endif

#define GF2_DIM 32

static unsigned long gf2_matrix_times(unsigned long *mat, unsigned long vec) {
  unsigned long sum;

  sum = 0;
  while (vec) {
    if (vec & 1)
      sum ^= *mat;
    vec >>= 1;
    mat++;
  }
  return sum;
}

static void gf2_matrix_square(unsigned long *square, unsigned long *mat) {
  int n;

  for (n = 0; n < GF2_DIM; n++)
    square[n] = gf2_matrix_times(mat, mat[n]);
}

static uLong crc32_combine_(uLong crc1, uLong crc2, z_off64_t len2) {
  int n;
  unsigned long row;
  unsigned long even[GF2_DIM];
  unsigned long odd[GF2_DIM];

  if (len2 <= 0)
    return crc1;

  odd[0] = 0xedb88320UL;
  row = 1;
  for (n = 1; n < GF2_DIM; n++) {
    odd[n] = row;
    row <<= 1;
  }

  gf2_matrix_square(even, odd);

  gf2_matrix_square(odd, even);

  do {

    gf2_matrix_square(even, odd);
    if (len2 & 1)
      crc1 = gf2_matrix_times(even, crc1);
    len2 >>= 1;

    if (len2 == 0)
      break;

    gf2_matrix_square(odd, even);
    if (len2 & 1)
      crc1 = gf2_matrix_times(odd, crc1);
    len2 >>= 1;

  } while (len2 != 0);

  crc1 ^= crc2;
  return crc1;
}

uLong ZEXPORT crc32_combine(uLong crc1, uLong crc2, z_off_t len2) {
  return crc32_combine_(crc1, crc2, len2);
}

uLong ZEXPORT crc32_combine64(uLong crc1, uLong crc2, z_off64_t len2) {
  return crc32_combine_(crc1, crc2, len2);
}

#endif
