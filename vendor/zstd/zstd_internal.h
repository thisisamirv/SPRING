/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_CCOMMON_H_MODULE
#define ZSTD_CCOMMON_H_MODULE

#include "compiler.h"
#include "cpu.h"
#include "debug.h"
#include "mem.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#define FSE_STATIC_LINKING_ONLY
#include "fse.h"
#include "huf.h"
#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY
#endif
#include "xxhash.h"
#ifndef ZSTD_NO_TRACE
#include "zstd_trace.h"
#else
#define ZSTD_TRACE 0
#endif

#define ZSTD_STATIC_ASSERT(c) DEBUG_STATIC_ASSERT(c)
#define ZSTD_isError ERR_isError
#define FSE_isError ERR_isError
#define HUF_isError ERR_isError

#undef MIN
#undef MAX
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define BOUNDED(min, val, max) (MAX(min, MIN(val, max)))

#define ZSTD_OPT_NUM (1 << 12)

#define ZSTD_REP_NUM 3
static UNUSED_ATTR const U32 repStartValue[ZSTD_REP_NUM] = {1, 4, 8};

#define KB *(1 << 10)
#define MB *(1 << 20)
#define GB *(1U << 30)

#define BIT7 128
#define BIT6 64
#define BIT5 32
#define BIT4 16
#define BIT1 2
#define BIT0 1

#define ZSTD_WINDOWLOG_ABSOLUTEMIN 10
static UNUSED_ATTR const size_t ZSTD_fcs_fieldSize[4] = {0, 2, 4, 8};
static UNUSED_ATTR const size_t ZSTD_did_fieldSize[4] = {0, 1, 2, 4};

#define ZSTD_FRAMEIDSIZE 4

#define ZSTD_BLOCKHEADERSIZE 3

static UNUSED_ATTR const size_t ZSTD_blockHeaderSize = ZSTD_BLOCKHEADERSIZE;
typedef enum { bt_raw, bt_rle, bt_compressed, bt_reserved } blockType_e;

#define ZSTD_FRAMECHECKSUMSIZE 4

#define MIN_SEQUENCES_SIZE 1
#define MIN_CBLOCK_SIZE (1 + 1)
#define MIN_LITERALS_FOR_4_STREAMS 6

typedef enum {
  set_basic,
  set_rle,
  set_compressed,
  set_repeat
} SymbolEncodingType_e;

#define LONGNBSEQ 0x7F00

#define MINMATCH 3

#define Litbits 8
#define LitHufLog 11
#define MaxLit ((1 << Litbits) - 1)
#define MaxML 52
#define MaxLL 35
#define DefaultMaxOff 28
#define MaxOff 31
#define MaxSeq MAX(MaxLL, MaxML)
#define MLFSELog 9
#define LLFSELog 9
#define OffFSELog 8
#define MaxFSELog MAX(MAX(MLFSELog, LLFSELog), OffFSELog)
#define MaxMLBits 16
#define MaxLLBits 16

#define ZSTD_MAX_HUF_HEADER_SIZE 128

#define ZSTD_MAX_FSE_HEADERS_SIZE                                              \
  (((MaxML + 1) * MLFSELog + (MaxLL + 1) * LLFSELog +                          \
    (MaxOff + 1) * OffFSELog + 7) /                                            \
   8)

static UNUSED_ATTR const U8 LL_bits[MaxLL + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  1,  1,
    1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static UNUSED_ATTR const S16 LL_defaultNorm[MaxLL + 1] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
    2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
#define LL_DEFAULTNORMLOG 6
static UNUSED_ATTR const U32 LL_defaultNormLog = LL_DEFAULTNORMLOG;

static UNUSED_ATTR const U8 ML_bits[MaxML + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  1,  1,  1, 1,
    2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static UNUSED_ATTR const S16 ML_defaultNorm[MaxML + 1] = {
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
#define ML_DEFAULTNORMLOG 6
static UNUSED_ATTR const U32 ML_defaultNormLog = ML_DEFAULTNORMLOG;

static UNUSED_ATTR const S16 OF_defaultNorm[DefaultMaxOff + 1] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
#define OF_DEFAULTNORMLOG 5
static UNUSED_ATTR const U32 OF_defaultNormLog = OF_DEFAULTNORMLOG;

static void ZSTD_copy8(void *dst, const void *src) {
#if defined(ZSTD_ARCH_ARM_NEON) && !defined(__aarch64__)
  vst1_u8((uint8_t *)dst, vld1_u8((const uint8_t *)src));
#else
  ZSTD_memcpy(dst, src, 8);
#endif
}
#define COPY8(d, s)                                                            \
  do {                                                                         \
    ZSTD_copy8(d, s);                                                          \
    d += 8;                                                                    \
    s += 8;                                                                    \
  } while (0)

static void ZSTD_copy16(void *dst, const void *src) {
#if defined(ZSTD_ARCH_ARM_NEON)
  vst1q_u8((uint8_t *)dst, vld1q_u8((const uint8_t *)src));
#elif defined(ZSTD_ARCH_X86_SSE2)
  _mm_storeu_si128((__m128i *)dst, _mm_loadu_si128((const __m128i *)src));
#elif defined(ZSTD_ARCH_RISCV_RVV)
  __riscv_vse8_v_u8m1((uint8_t *)dst,
                      __riscv_vle8_v_u8m1((const uint8_t *)src, 16), 16);
#elif defined(__clang__)
  ZSTD_memmove(dst, src, 16);
#else

  BYTE copy16_buf[16];
  ZSTD_memcpy(copy16_buf, src, 16);
  ZSTD_memcpy(dst, copy16_buf, 16);
#endif
}
#define COPY16(d, s)                                                           \
  do {                                                                         \
    ZSTD_copy16(d, s);                                                         \
    d += 16;                                                                   \
    s += 16;                                                                   \
  } while (0)

#define WILDCOPY_OVERLENGTH 32
#define WILDCOPY_VECLEN 16

typedef enum {
  ZSTD_no_overlap,
  ZSTD_overlap_src_before_dst

} ZSTD_overlap_e;

MEM_STATIC FORCE_INLINE_ATTR void ZSTD_wildcopy(void *dst, const void *src,
                                                size_t length,
                                                ZSTD_overlap_e const ovtype) {
  ptrdiff_t diff = (BYTE *)dst - (const BYTE *)src;
  const BYTE *ip = (const BYTE *)src;
  BYTE *op = (BYTE *)dst;
  BYTE *const oend = op + length;

  if (ovtype == ZSTD_overlap_src_before_dst && diff < WILDCOPY_VECLEN) {

    do {
      COPY8(op, ip);
    } while (op < oend);
  } else {
    assert(diff >= WILDCOPY_VECLEN || diff <= -WILDCOPY_VECLEN);

    ZSTD_copy16(op, ip);
    if (16 >= length)
      return;
    op += 16;
    ip += 16;
    do {
      COPY16(op, ip);
      COPY16(op, ip);
    } while (op < oend);
  }
}

MEM_STATIC size_t ZSTD_limitCopy(void *dst, size_t dstCapacity, const void *src,
                                 size_t srcSize) {
  size_t const length = MIN(dstCapacity, srcSize);
  if (length > 0) {
    ZSTD_memcpy(dst, src, length);
  }
  return length;
}

#define ZSTD_WORKSPACETOOLARGE_FACTOR 3

#define ZSTD_WORKSPACETOOLARGE_MAXDURATION 128

typedef enum { ZSTD_bm_buffered = 0, ZSTD_bm_stable = 1 } ZSTD_bufferMode_e;

typedef struct {
  size_t nbBlocks;
  size_t compressedSize;
  unsigned long long decompressedBound;
} ZSTD_frameSizeInfo;

void ZSTD_invalidateRepCodes(ZSTD_CCtx *cctx);

typedef struct {
  blockType_e blockType;
  U32 lastBlock;
  U32 origSize;
} blockProperties_t;

size_t ZSTD_getcBlockSize(const void *src, size_t srcSize,
                          blockProperties_t *bpPtr);

size_t ZSTD_decodeSeqHeaders(ZSTD_DCtx *dctx, int *nbSeqPtr, const void *src,
                             size_t srcSize);

MEM_STATIC int ZSTD_cpuSupportsBmi2(void) {
  ZSTD_cpuid_t cpuid = ZSTD_cpuid();
  return ZSTD_cpuid_bmi1(cpuid) && ZSTD_cpuid_bmi2(cpuid);
}

#endif
