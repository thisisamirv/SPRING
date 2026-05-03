/* ******************************************************************
 * huff0 huffman decoder,
 * part of Finite State Entropy library
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 *  You can contact the author at :
 *  - FSE+HUF source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 ****************************************************************** */

#include "bits.h"
#include "bitstream.h"
#include "compiler.h"
#include "error_private.h"
#include "fse.h"
#include "huf.h"
#include "zstd_deps.h"
#include "zstd_internal.h"
#include <stddef.h>

#define HUF_DECODER_FAST_TABLELOG 11

#ifdef HUF_DISABLE_FAST_DECODE
#define HUF_ENABLE_FAST_DECODE 0
#else
#define HUF_ENABLE_FAST_DECODE 1
#endif

#if defined(HUF_FORCE_DECOMPRESS_X1) && defined(HUF_FORCE_DECOMPRESS_X2)
#error "Cannot force the use of the X1 and X2 decoders at the same time!"
#endif

#if DYNAMIC_BMI2
#define HUF_FAST_BMI2_ATTRS BMI2_TARGET_ATTRIBUTE
#else
#define HUF_FAST_BMI2_ATTRS
#endif

#ifdef __cplusplus
#define HUF_EXTERN_C extern "C"
#else
#define HUF_EXTERN_C
#endif
#define HUF_ASM_DECL HUF_EXTERN_C

#if DYNAMIC_BMI2
#define HUF_NEED_BMI2_FUNCTION 1
#else
#define HUF_NEED_BMI2_FUNCTION 0
#endif

#define HUF_isError ERR_isError

#define HUF_ALIGN(x, a) HUF_ALIGN_MASK((x), (a) - 1)
#define HUF_ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

typedef size_t (*HUF_DecompressUsingDTableFn)(void *dst, size_t dstSize,
                                              const void *cSrc, size_t cSrcSize,
                                              const HUF_DTable *DTable);

#if DYNAMIC_BMI2

#define HUF_DGEN(fn)                                                           \
                                                                               \
  static size_t fn##_default(void *dst, size_t dstSize, const void *cSrc,      \
                             size_t cSrcSize, const HUF_DTable *DTable) {      \
    return fn##_body(dst, dstSize, cSrc, cSrcSize, DTable);                    \
  }                                                                            \
                                                                               \
  static BMI2_TARGET_ATTRIBUTE size_t fn##_bmi2(                               \
      void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,            \
      const HUF_DTable *DTable) {                                              \
    return fn##_body(dst, dstSize, cSrc, cSrcSize, DTable);                    \
  }                                                                            \
                                                                               \
  static size_t fn(void *dst, size_t dstSize, void const *cSrc,                \
                   size_t cSrcSize, HUF_DTable const *DTable, int flags) {     \
    if (flags & HUF_flags_bmi2) {                                              \
      return fn##_bmi2(dst, dstSize, cSrc, cSrcSize, DTable);                  \
    }                                                                          \
    return fn##_default(dst, dstSize, cSrc, cSrcSize, DTable);                 \
  }

#else

#define HUF_DGEN(fn)                                                           \
  static size_t fn(void *dst, size_t dstSize, void const *cSrc,                \
                   size_t cSrcSize, HUF_DTable const *DTable, int flags) {     \
    (void)flags;                                                               \
    return fn##_body(dst, dstSize, cSrc, cSrcSize, DTable);                    \
  }

#endif

typedef struct {
  BYTE maxTableLog;
  BYTE tableType;
  BYTE tableLog;
  BYTE reserved;
} DTableDesc;

static DTableDesc HUF_getDTableDesc(const HUF_DTable *table) {
  DTableDesc dtd;
  ZSTD_memcpy(&dtd, table, sizeof(dtd));
  return dtd;
}

static size_t HUF_initFastDStream(BYTE const *ip) {
  BYTE const lastByte = ip[7];
  size_t const bitsConsumed = lastByte ? 8 - ZSTD_highbit32(lastByte) : 0;
  size_t const value = MEM_readLEST(ip) | 1;
  assert(bitsConsumed <= 8);
  assert(sizeof(size_t) == 8);
  return value << bitsConsumed;
}

typedef struct {
  BYTE const *ip[4];
  BYTE *op[4];
  U64 bits[4];
  void const *dt;
  BYTE const *ilowest;
  BYTE *oend;
  BYTE const *iend[4];
} HUF_DecompressFastArgs;

typedef void (*HUF_DecompressFastLoopFn)(HUF_DecompressFastArgs *);

static size_t HUF_DecompressFastArgs_init(HUF_DecompressFastArgs *args,
                                          void *dst, size_t dstSize,
                                          void const *src, size_t srcSize,
                                          const HUF_DTable *DTable) {
  void const *dt = DTable + 1;
  U32 const dtLog = HUF_getDTableDesc(DTable).tableLog;

  const BYTE *const istart = (const BYTE *)src;

  BYTE *const oend = (BYTE *)ZSTD_maybeNullPtrAdd(dst, (ptrdiff_t)dstSize);

  if (!MEM_isLittleEndian() || MEM_32bits())
    return 0;

  if (dstSize == 0)
    return 0;
  assert(dst != NULL);

  if (srcSize < 10)
    return ERROR(corruption_detected);

  if (dtLog != HUF_DECODER_FAST_TABLELOG)
    return 0;

  {
    size_t const length1 = MEM_readLE16(istart);
    size_t const length2 = MEM_readLE16(istart + 2);
    size_t const length3 = MEM_readLE16(istart + 4);
    size_t const length4 = srcSize - (length1 + length2 + length3 + 6);
    args->iend[0] = istart + 6;
    args->iend[1] = args->iend[0] + length1;
    args->iend[2] = args->iend[1] + length2;
    args->iend[3] = args->iend[2] + length3;

    if (length1 < 8 || length2 < 8 || length3 < 8 || length4 < 8)
      return 0;
    if (length4 > srcSize)
      return ERROR(corruption_detected);
  }

  args->ip[0] = args->iend[1] - sizeof(U64);
  args->ip[1] = args->iend[2] - sizeof(U64);
  args->ip[2] = args->iend[3] - sizeof(U64);
  args->ip[3] = (BYTE const *)src + srcSize - sizeof(U64);

  args->op[0] = (BYTE *)dst;
  args->op[1] = args->op[0] + (dstSize + 3) / 4;
  args->op[2] = args->op[1] + (dstSize + 3) / 4;
  args->op[3] = args->op[2] + (dstSize + 3) / 4;

  if (args->op[3] >= oend)
    return 0;

  args->bits[0] = HUF_initFastDStream(args->ip[0]);
  args->bits[1] = HUF_initFastDStream(args->ip[1]);
  args->bits[2] = HUF_initFastDStream(args->ip[2]);
  args->bits[3] = HUF_initFastDStream(args->ip[3]);

  args->ilowest = istart;

  args->oend = oend;
  args->dt = dt;

  return 1;
}

static size_t HUF_initRemainingDStream(BIT_DStream_t *bit,
                                       HUF_DecompressFastArgs const *args,
                                       int stream, BYTE *segmentEnd) {

  if (args->op[stream] > segmentEnd)
    return ERROR(corruption_detected);

  if (args->ip[stream] < args->iend[stream] - 8)
    return ERROR(corruption_detected);

  assert(sizeof(size_t) == 8);
  bit->bitContainer = MEM_readLEST(args->ip[stream]);
  bit->bitsConsumed = ZSTD_countTrailingZeros64(args->bits[stream]);
  bit->start = (const char *)args->ilowest;
  bit->limitPtr = bit->start + sizeof(size_t);
  bit->ptr = (const char *)args->ip[stream];

  return 0;
}

#define HUF_4X_FOR_EACH_STREAM(X)                                              \
  do {                                                                         \
    X(0);                                                                      \
    X(1);                                                                      \
    X(2);                                                                      \
    X(3);                                                                      \
  } while (0)

#define HUF_4X_FOR_EACH_STREAM_WITH_VAR(X, var)                                \
  do {                                                                         \
    X(0, (var));                                                               \
    X(1, (var));                                                               \
    X(2, (var));                                                               \
    X(3, (var));                                                               \
  } while (0)

#ifndef HUF_FORCE_DECOMPRESS_X2

typedef struct {
  BYTE nbBits;
  BYTE byte;
} HUF_DEltX1;

static U64 HUF_DEltX1_set4(BYTE symbol, BYTE nbBits) {
  U64 D4;
  if (MEM_isLittleEndian()) {
    D4 = (U64)((symbol << 8) + nbBits);
  } else {
    D4 = (U64)(symbol + (nbBits << 8));
  }
  assert(D4 < (1U << 16));
  D4 *= 0x0001000100010001ULL;
  return D4;
}

static U32 HUF_rescaleStats(BYTE *huffWeight, U32 *rankVal, U32 nbSymbols,
                            U32 tableLog, U32 targetTableLog) {
  if (tableLog > targetTableLog)
    return tableLog;
  if (tableLog < targetTableLog) {
    U32 const scale = targetTableLog - tableLog;
    U32 s;

    for (s = 0; s < nbSymbols; ++s) {
      huffWeight[s] += (BYTE)((huffWeight[s] == 0) ? 0 : scale);
    }

    for (s = targetTableLog; s > scale; --s) {
      rankVal[s] = rankVal[s - scale];
    }
    for (s = scale; s > 0; --s) {
      rankVal[s] = 0;
    }
  }
  return targetTableLog;
}

typedef struct {
  U32 rankVal[HUF_TABLELOG_ABSOLUTEMAX + 1];
  U32 rankStart[HUF_TABLELOG_ABSOLUTEMAX + 1];
  U32 statsWksp[HUF_READ_STATS_WORKSPACE_SIZE_U32];
  BYTE symbols[HUF_SYMBOLVALUE_MAX + 1];
  BYTE huffWeight[HUF_SYMBOLVALUE_MAX + 1];
} HUF_ReadDTableX1_Workspace;

size_t HUF_readDTableX1_wksp(HUF_DTable *DTable, const void *src,
                             size_t srcSize, void *workSpace, size_t wkspSize,
                             int flags) {
  U32 tableLog = 0;
  U32 nbSymbols = 0;
  size_t iSize;
  void *const dtPtr = DTable + 1;
  HUF_DEltX1 *const dt = (HUF_DEltX1 *)dtPtr;
  HUF_ReadDTableX1_Workspace *wksp = (HUF_ReadDTableX1_Workspace *)workSpace;

  DEBUG_STATIC_ASSERT(HUF_DECOMPRESS_WORKSPACE_SIZE >= sizeof(*wksp));
  if (sizeof(*wksp) > wkspSize)
    return ERROR(tableLog_tooLarge);

  DEBUG_STATIC_ASSERT(sizeof(DTableDesc) == sizeof(HUF_DTable));

  iSize = HUF_readStats_wksp(wksp->huffWeight, HUF_SYMBOLVALUE_MAX + 1,
                             wksp->rankVal, &nbSymbols, &tableLog, src, srcSize,
                             wksp->statsWksp, sizeof(wksp->statsWksp), flags);
  if (HUF_isError(iSize))
    return iSize;

  {
    DTableDesc dtd = HUF_getDTableDesc(DTable);
    U32 const maxTableLog = dtd.maxTableLog + 1;
    U32 const targetTableLog = MIN(maxTableLog, HUF_DECODER_FAST_TABLELOG);
    tableLog = HUF_rescaleStats(wksp->huffWeight, wksp->rankVal, nbSymbols,
                                tableLog, targetTableLog);
    if (tableLog > (U32)(dtd.maxTableLog + 1))
      return ERROR(tableLog_tooLarge);
    dtd.tableType = 0;
    dtd.tableLog = (BYTE)tableLog;
    ZSTD_memcpy(DTable, &dtd, sizeof(dtd));
  }

  {
    int n;
    U32 nextRankStart = 0;
    int const unroll = 4;
    int const nLimit = (int)nbSymbols - unroll + 1;
    for (n = 0; n < (int)tableLog + 1; n++) {
      U32 const curr = nextRankStart;
      nextRankStart += wksp->rankVal[n];
      wksp->rankStart[n] = curr;
    }
    for (n = 0; n < nLimit; n += unroll) {
      int u;
      for (u = 0; u < unroll; ++u) {
        size_t const w = wksp->huffWeight[n + u];
        wksp->symbols[wksp->rankStart[w]++] = (BYTE)(n + u);
      }
    }
    for (; n < (int)nbSymbols; ++n) {
      size_t const w = wksp->huffWeight[n];
      wksp->symbols[wksp->rankStart[w]++] = (BYTE)n;
    }
  }

  {
    U32 w;
    int symbol = wksp->rankVal[0];
    int rankStart = 0;
    for (w = 1; w < tableLog + 1; ++w) {
      int const symbolCount = wksp->rankVal[w];
      int const length = (1 << w) >> 1;
      int uStart = rankStart;
      BYTE const nbBits = (BYTE)(tableLog + 1 - w);
      int s;
      int u;
      switch (length) {
      case 1:
        for (s = 0; s < symbolCount; ++s) {
          HUF_DEltX1 D;
          D.byte = wksp->symbols[symbol + s];
          D.nbBits = nbBits;
          dt[uStart] = D;
          uStart += 1;
        }
        break;
      case 2:
        for (s = 0; s < symbolCount; ++s) {
          HUF_DEltX1 D;
          D.byte = wksp->symbols[symbol + s];
          D.nbBits = nbBits;
          dt[uStart + 0] = D;
          dt[uStart + 1] = D;
          uStart += 2;
        }
        break;
      case 4:
        for (s = 0; s < symbolCount; ++s) {
          U64 const D4 = HUF_DEltX1_set4(wksp->symbols[symbol + s], nbBits);
          MEM_write64(dt + uStart, D4);
          uStart += 4;
        }
        break;
      case 8:
        for (s = 0; s < symbolCount; ++s) {
          U64 const D4 = HUF_DEltX1_set4(wksp->symbols[symbol + s], nbBits);
          MEM_write64(dt + uStart, D4);
          MEM_write64(dt + uStart + 4, D4);
          uStart += 8;
        }
        break;
      default:
        for (s = 0; s < symbolCount; ++s) {
          U64 const D4 = HUF_DEltX1_set4(wksp->symbols[symbol + s], nbBits);
          for (u = 0; u < length; u += 16) {
            MEM_write64(dt + uStart + u + 0, D4);
            MEM_write64(dt + uStart + u + 4, D4);
            MEM_write64(dt + uStart + u + 8, D4);
            MEM_write64(dt + uStart + u + 12, D4);
          }
          assert(u == length);
          uStart += length;
        }
        break;
      }
      symbol += symbolCount;
      rankStart += symbolCount * length;
    }
  }
  return iSize;
}

FORCE_INLINE_TEMPLATE BYTE HUF_decodeSymbolX1(BIT_DStream_t *Dstream,
                                              const HUF_DEltX1 *dt,
                                              const U32 dtLog) {
  size_t const val = BIT_lookBitsFast(Dstream, dtLog);
  BYTE const c = dt[val].byte;
  BIT_skipBits(Dstream, dt[val].nbBits);
  return c;
}

#define HUF_DECODE_SYMBOLX1_0(ptr, DStreamPtr)                                 \
  do {                                                                         \
    *ptr++ = HUF_decodeSymbolX1(DStreamPtr, dt, dtLog);                        \
  } while (0)

#define HUF_DECODE_SYMBOLX1_1(ptr, DStreamPtr)                                 \
  do {                                                                         \
    if (MEM_64bits() || (HUF_TABLELOG_MAX <= 12))                              \
      HUF_DECODE_SYMBOLX1_0(ptr, DStreamPtr);                                  \
  } while (0)

#define HUF_DECODE_SYMBOLX1_2(ptr, DStreamPtr)                                 \
  do {                                                                         \
    if (MEM_64bits())                                                          \
      HUF_DECODE_SYMBOLX1_0(ptr, DStreamPtr);                                  \
  } while (0)

HINT_INLINE size_t HUF_decodeStreamX1(BYTE *p, BIT_DStream_t *const bitDPtr,
                                      BYTE *const pEnd,
                                      const HUF_DEltX1 *const dt,
                                      const U32 dtLog) {
  BYTE *const pStart = p;

  if ((pEnd - p) > 3) {
    while ((BIT_reloadDStream(bitDPtr) == BIT_DStream_unfinished) &
           (p < pEnd - 3)) {
      HUF_DECODE_SYMBOLX1_2(p, bitDPtr);
      HUF_DECODE_SYMBOLX1_1(p, bitDPtr);
      HUF_DECODE_SYMBOLX1_2(p, bitDPtr);
      HUF_DECODE_SYMBOLX1_0(p, bitDPtr);
    }
  } else {
    BIT_reloadDStream(bitDPtr);
  }

  if (MEM_32bits())
    while ((BIT_reloadDStream(bitDPtr) == BIT_DStream_unfinished) & (p < pEnd))
      HUF_DECODE_SYMBOLX1_0(p, bitDPtr);

  while (p < pEnd)
    HUF_DECODE_SYMBOLX1_0(p, bitDPtr);

  return (size_t)(pEnd - pStart);
}

FORCE_INLINE_TEMPLATE size_t HUF_decompress1X1_usingDTable_internal_body(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable) {
  BYTE *op = (BYTE *)dst;
  BYTE *const oend = (BYTE *)ZSTD_maybeNullPtrAdd(op, (ptrdiff_t)dstSize);
  const void *dtPtr = DTable + 1;
  const HUF_DEltX1 *const dt = (const HUF_DEltX1 *)dtPtr;
  BIT_DStream_t bitD;
  DTableDesc const dtd = HUF_getDTableDesc(DTable);
  U32 const dtLog = dtd.tableLog;

  CHECK_F(BIT_initDStream(&bitD, cSrc, cSrcSize));

  HUF_decodeStreamX1(op, &bitD, oend, dt, dtLog);

  if (!BIT_endOfDStream(&bitD))
    return ERROR(corruption_detected);

  return dstSize;
}

FORCE_INLINE_TEMPLATE size_t HUF_decompress4X1_usingDTable_internal_body(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable) {

  if (cSrcSize < 10)
    return ERROR(corruption_detected);

  if (dstSize < 6)
    return ERROR(corruption_detected);

  {
    const BYTE *const istart = (const BYTE *)cSrc;
    BYTE *const ostart = (BYTE *)dst;
    BYTE *const oend = ostart + dstSize;
    BYTE *const olimit = oend - 3;
    const void *const dtPtr = DTable + 1;
    const HUF_DEltX1 *const dt = (const HUF_DEltX1 *)dtPtr;

    BIT_DStream_t bitD1;
    BIT_DStream_t bitD2;
    BIT_DStream_t bitD3;
    BIT_DStream_t bitD4;
    size_t const length1 = MEM_readLE16(istart);
    size_t const length2 = MEM_readLE16(istart + 2);
    size_t const length3 = MEM_readLE16(istart + 4);
    size_t const length4 = cSrcSize - (length1 + length2 + length3 + 6);
    const BYTE *const istart1 = istart + 6;
    const BYTE *const istart2 = istart1 + length1;
    const BYTE *const istart3 = istart2 + length2;
    const BYTE *const istart4 = istart3 + length3;
    const size_t segmentSize = (dstSize + 3) / 4;
    BYTE *const opStart2 = ostart + segmentSize;
    BYTE *const opStart3 = opStart2 + segmentSize;
    BYTE *const opStart4 = opStart3 + segmentSize;
    BYTE *op1 = ostart;
    BYTE *op2 = opStart2;
    BYTE *op3 = opStart3;
    BYTE *op4 = opStart4;
    DTableDesc const dtd = HUF_getDTableDesc(DTable);
    U32 const dtLog = dtd.tableLog;
    U32 endSignal = 1;

    if (length4 > cSrcSize)
      return ERROR(corruption_detected);
    if (opStart4 > oend)
      return ERROR(corruption_detected);
    assert(dstSize >= 6);
    CHECK_F(BIT_initDStream(&bitD1, istart1, length1));
    CHECK_F(BIT_initDStream(&bitD2, istart2, length2));
    CHECK_F(BIT_initDStream(&bitD3, istart3, length3));
    CHECK_F(BIT_initDStream(&bitD4, istart4, length4));

    if ((size_t)(oend - op4) >= sizeof(size_t)) {
      for (; (endSignal) & (op4 < olimit);) {
        HUF_DECODE_SYMBOLX1_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX1_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX1_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX1_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX1_1(op1, &bitD1);
        HUF_DECODE_SYMBOLX1_1(op2, &bitD2);
        HUF_DECODE_SYMBOLX1_1(op3, &bitD3);
        HUF_DECODE_SYMBOLX1_1(op4, &bitD4);
        HUF_DECODE_SYMBOLX1_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX1_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX1_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX1_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX1_0(op1, &bitD1);
        HUF_DECODE_SYMBOLX1_0(op2, &bitD2);
        HUF_DECODE_SYMBOLX1_0(op3, &bitD3);
        HUF_DECODE_SYMBOLX1_0(op4, &bitD4);
        endSignal &= BIT_reloadDStreamFast(&bitD1) == BIT_DStream_unfinished;
        endSignal &= BIT_reloadDStreamFast(&bitD2) == BIT_DStream_unfinished;
        endSignal &= BIT_reloadDStreamFast(&bitD3) == BIT_DStream_unfinished;
        endSignal &= BIT_reloadDStreamFast(&bitD4) == BIT_DStream_unfinished;
      }
    }

    if (op1 > opStart2)
      return ERROR(corruption_detected);
    if (op2 > opStart3)
      return ERROR(corruption_detected);
    if (op3 > opStart4)
      return ERROR(corruption_detected);

    HUF_decodeStreamX1(op1, &bitD1, opStart2, dt, dtLog);
    HUF_decodeStreamX1(op2, &bitD2, opStart3, dt, dtLog);
    HUF_decodeStreamX1(op3, &bitD3, opStart4, dt, dtLog);
    HUF_decodeStreamX1(op4, &bitD4, oend, dt, dtLog);

    {
      U32 const endCheck = BIT_endOfDStream(&bitD1) & BIT_endOfDStream(&bitD2) &
                           BIT_endOfDStream(&bitD3) & BIT_endOfDStream(&bitD4);
      if (!endCheck)
        return ERROR(corruption_detected);
    }

    return dstSize;
  }
}

#if HUF_NEED_BMI2_FUNCTION
static BMI2_TARGET_ATTRIBUTE size_t HUF_decompress4X1_usingDTable_internal_bmi2(
    void *dst, size_t dstSize, void const *cSrc, size_t cSrcSize,
    HUF_DTable const *DTable) {
  return HUF_decompress4X1_usingDTable_internal_body(dst, dstSize, cSrc,
                                                     cSrcSize, DTable);
}
#endif

static size_t HUF_decompress4X1_usingDTable_internal_default(
    void *dst, size_t dstSize, void const *cSrc, size_t cSrcSize,
    HUF_DTable const *DTable) {
  return HUF_decompress4X1_usingDTable_internal_body(dst, dstSize, cSrc,
                                                     cSrcSize, DTable);
}

#if ZSTD_ENABLE_ASM_X86_64_BMI2

HUF_ASM_DECL void HUF_decompress4X1_usingDTable_internal_fast_asm_loop(
    HUF_DecompressFastArgs *args) ZSTDLIB_HIDDEN;

#endif

static HUF_FAST_BMI2_ATTRS void
HUF_decompress4X1_usingDTable_internal_fast_c_loop(
    HUF_DecompressFastArgs *args) {
  U64 bits[4];
  BYTE const *ip[4];
  BYTE *op[4];
  U16 const *const dtable = (U16 const *)args->dt;
  BYTE *const oend = args->oend;
  BYTE const *const ilowest = args->ilowest;

  ZSTD_memcpy(&bits, &args->bits, sizeof(bits));
  ZSTD_memcpy((void *)(&ip), &args->ip, sizeof(ip));
  ZSTD_memcpy(&op, &args->op, sizeof(op));

  assert(MEM_isLittleEndian());
  assert(!MEM_32bits());

  for (;;) {
    BYTE *olimit;
    int stream;

#ifndef NDEBUG
    for (stream = 0; stream < 4; ++stream) {
      assert(op[stream] <= (stream == 3 ? oend : op[stream + 1]));
      assert(ip[stream] >= ilowest);
    }
#endif

    {

      size_t const oiters = (size_t)(oend - op[3]) / 5;

      size_t const iiters = (size_t)(ip[0] - ilowest) / 7;

      size_t const iters = MIN(oiters, iiters);
      size_t const symbols = iters * 5;

      olimit = op[3] + symbols;

      if (op[3] == olimit)
        break;

      for (stream = 1; stream < 4; ++stream) {
        if (ip[stream] < ip[stream - 1])
          goto _out;
      }
    }

#ifndef NDEBUG
    for (stream = 1; stream < 4; ++stream) {
      assert(ip[stream] >= ip[stream - 1]);
    }
#endif

#define HUF_4X1_DECODE_SYMBOL(_stream, _symbol)                                \
  do {                                                                         \
    U64 const index = bits[(_stream)] >> 53;                                   \
    U16 const entry = dtable[index];                                           \
    bits[(_stream)] <<= entry & 0x3F;                                          \
    op[(_stream)][(_symbol)] = (BYTE)(entry >> 8);                             \
  } while (0)

#define HUF_5X1_RELOAD_STREAM(_stream)                                         \
  do {                                                                         \
    U64 const ctz = ZSTD_countTrailingZeros64(bits[(_stream)]);                \
    U64 const nbBits = ctz & 7;                                                \
    U64 const nbBytes = ctz >> 3;                                              \
    op[(_stream)] += 5;                                                        \
    ip[(_stream)] -= nbBytes;                                                  \
    bits[(_stream)] = MEM_read64(ip[(_stream)]) | 1;                           \
    bits[(_stream)] <<= nbBits;                                                \
  } while (0)

    do {

      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X1_DECODE_SYMBOL, 0);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X1_DECODE_SYMBOL, 1);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X1_DECODE_SYMBOL, 2);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X1_DECODE_SYMBOL, 3);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X1_DECODE_SYMBOL, 4);

      HUF_4X_FOR_EACH_STREAM(HUF_5X1_RELOAD_STREAM);
    } while (op[3] < olimit);

#undef HUF_4X1_DECODE_SYMBOL
#undef HUF_5X1_RELOAD_STREAM
  }

_out:

  ZSTD_memcpy(&args->bits, &bits, sizeof(bits));
  ZSTD_memcpy((void *)(&args->ip), &ip, sizeof(ip));
  ZSTD_memcpy(&args->op, &op, sizeof(op));
}

static HUF_FAST_BMI2_ATTRS size_t HUF_decompress4X1_usingDTable_internal_fast(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable, HUF_DecompressFastLoopFn loopFn) {
  void const *dt = DTable + 1;
  BYTE const *const ilowest = (BYTE const *)cSrc;
  BYTE *const oend = (BYTE *)ZSTD_maybeNullPtrAdd(dst, (ptrdiff_t)dstSize);
  HUF_DecompressFastArgs args;
  {
    size_t const ret = HUF_DecompressFastArgs_init(&args, dst, dstSize, cSrc,
                                                   cSrcSize, DTable);
    FORWARD_IF_ERROR(ret, "Failed to init fast loop args");
    if (ret == 0)
      return 0;
  }

  assert(args.ip[0] >= args.ilowest);
  loopFn(&args);

  assert(args.ip[0] >= ilowest);
  assert(args.ip[0] >= ilowest);
  assert(args.ip[1] >= ilowest);
  assert(args.ip[2] >= ilowest);
  assert(args.ip[3] >= ilowest);
  assert(args.op[3] <= oend);

  assert(ilowest == args.ilowest);
  assert(ilowest + 6 == args.iend[0]);
  (void)ilowest;

  {
    size_t const segmentSize = (dstSize + 3) / 4;
    BYTE *segmentEnd = (BYTE *)dst;
    int i;
    for (i = 0; i < 4; ++i) {
      BIT_DStream_t bit;
      if (segmentSize <= (size_t)(oend - segmentEnd))
        segmentEnd += segmentSize;
      else
        segmentEnd = oend;
      FORWARD_IF_ERROR(HUF_initRemainingDStream(&bit, &args, i, segmentEnd),
                       "corruption");

      args.op[i] +=
          HUF_decodeStreamX1(args.op[i], &bit, segmentEnd,
                             (HUF_DEltX1 const *)dt, HUF_DECODER_FAST_TABLELOG);
      if (args.op[i] != segmentEnd)
        return ERROR(corruption_detected);
    }
  }

  assert(dstSize != 0);
  return dstSize;
}

HUF_DGEN(HUF_decompress1X1_usingDTable_internal)

static size_t HUF_decompress4X1_usingDTable_internal(void *dst, size_t dstSize,
                                                     void const *cSrc,
                                                     size_t cSrcSize,
                                                     HUF_DTable const *DTable,
                                                     int flags) {
  HUF_DecompressUsingDTableFn fallbackFn =
      HUF_decompress4X1_usingDTable_internal_default;
  HUF_DecompressFastLoopFn loopFn =
      HUF_decompress4X1_usingDTable_internal_fast_c_loop;

#if DYNAMIC_BMI2
  if (flags & HUF_flags_bmi2) {
    fallbackFn = HUF_decompress4X1_usingDTable_internal_bmi2;
#if ZSTD_ENABLE_ASM_X86_64_BMI2
    if (!(flags & HUF_flags_disableAsm)) {
      loopFn = HUF_decompress4X1_usingDTable_internal_fast_asm_loop;
    }
#endif
  } else {
    return fallbackFn(dst, dstSize, cSrc, cSrcSize, DTable);
  }
#endif

#if ZSTD_ENABLE_ASM_X86_64_BMI2 && defined(__BMI2__)
  if (!(flags & HUF_flags_disableAsm)) {
    loopFn = HUF_decompress4X1_usingDTable_internal_fast_asm_loop;
  }
#endif

  if (HUF_ENABLE_FAST_DECODE && !(flags & HUF_flags_disableFast)) {
    size_t const ret = HUF_decompress4X1_usingDTable_internal_fast(
        dst, dstSize, cSrc, cSrcSize, DTable, loopFn);
    if (ret != 0)
      return ret;
  }
  return fallbackFn(dst, dstSize, cSrc, cSrcSize, DTable);
}

static size_t HUF_decompress4X1_DCtx_wksp(HUF_DTable *dctx, void *dst,
                                          size_t dstSize, const void *cSrc,
                                          size_t cSrcSize, void *workSpace,
                                          size_t wkspSize, int flags) {
  const BYTE *ip = (const BYTE *)cSrc;

  size_t const hSize =
      HUF_readDTableX1_wksp(dctx, cSrc, cSrcSize, workSpace, wkspSize, flags);
  if (HUF_isError(hSize))
    return hSize;
  if (hSize >= cSrcSize)
    return ERROR(srcSize_wrong);
  ip += hSize;
  cSrcSize -= hSize;

  return HUF_decompress4X1_usingDTable_internal(dst, dstSize, ip, cSrcSize,
                                                dctx, flags);
}

#endif

#ifndef HUF_FORCE_DECOMPRESS_X1

typedef struct {
  U16 sequence;
  BYTE nbBits;
  BYTE length;
} HUF_DEltX2;
typedef struct {
  BYTE symbol;
} sortedSymbol_t;
typedef U32 rankValCol_t[HUF_TABLELOG_MAX + 1];
typedef rankValCol_t rankVal_t[HUF_TABLELOG_MAX];

static U32 HUF_buildDEltX2U32(U32 symbol, U32 nbBits, U32 baseSeq, int level) {
  U32 seq;
  DEBUG_STATIC_ASSERT(offsetof(HUF_DEltX2, sequence) == 0);
  DEBUG_STATIC_ASSERT(offsetof(HUF_DEltX2, nbBits) == 2);
  DEBUG_STATIC_ASSERT(offsetof(HUF_DEltX2, length) == 3);
  DEBUG_STATIC_ASSERT(sizeof(HUF_DEltX2) == sizeof(U32));
  if (MEM_isLittleEndian()) {
    seq = level == 1 ? symbol : (baseSeq + (symbol << 8));
    return seq + (nbBits << 16) + ((U32)level << 24);
  } else {
    seq = level == 1 ? (symbol << 8) : ((baseSeq << 8) + symbol);
    return (seq << 16) + (nbBits << 8) + (U32)level;
  }
}

static HUF_DEltX2 HUF_buildDEltX2(U32 symbol, U32 nbBits, U32 baseSeq,
                                  int level) {
  HUF_DEltX2 DElt;
  U32 const val = HUF_buildDEltX2U32(symbol, nbBits, baseSeq, level);
  DEBUG_STATIC_ASSERT(sizeof(DElt) == sizeof(val));
  ZSTD_memcpy(&DElt, &val, sizeof(val));
  return DElt;
}

static U64 HUF_buildDEltX2U64(U32 symbol, U32 nbBits, U16 baseSeq, int level) {
  U32 DElt = HUF_buildDEltX2U32(symbol, nbBits, baseSeq, level);
  return (U64)DElt + ((U64)DElt << 32);
}

static void HUF_fillDTableX2ForWeight(HUF_DEltX2 *DTableRank,
                                      sortedSymbol_t const *begin,
                                      sortedSymbol_t const *end, U32 nbBits,
                                      U32 tableLog, U16 baseSeq,
                                      int const level) {
  U32 const length = 1U << ((tableLog - nbBits) & 0x1F);
  const sortedSymbol_t *ptr;
  assert(level >= 1 && level <= 2);
  switch (length) {
  case 1:
    for (ptr = begin; ptr != end; ++ptr) {
      HUF_DEltX2 const DElt =
          HUF_buildDEltX2(ptr->symbol, nbBits, baseSeq, level);
      *DTableRank++ = DElt;
    }
    break;
  case 2:
    for (ptr = begin; ptr != end; ++ptr) {
      HUF_DEltX2 const DElt =
          HUF_buildDEltX2(ptr->symbol, nbBits, baseSeq, level);
      DTableRank[0] = DElt;
      DTableRank[1] = DElt;
      DTableRank += 2;
    }
    break;
  case 4:
    for (ptr = begin; ptr != end; ++ptr) {
      U64 const DEltX2 =
          HUF_buildDEltX2U64(ptr->symbol, nbBits, baseSeq, level);
      ZSTD_memcpy(DTableRank + 0, &DEltX2, sizeof(DEltX2));
      ZSTD_memcpy(DTableRank + 2, &DEltX2, sizeof(DEltX2));
      DTableRank += 4;
    }
    break;
  case 8:
    for (ptr = begin; ptr != end; ++ptr) {
      U64 const DEltX2 =
          HUF_buildDEltX2U64(ptr->symbol, nbBits, baseSeq, level);
      ZSTD_memcpy(DTableRank + 0, &DEltX2, sizeof(DEltX2));
      ZSTD_memcpy(DTableRank + 2, &DEltX2, sizeof(DEltX2));
      ZSTD_memcpy(DTableRank + 4, &DEltX2, sizeof(DEltX2));
      ZSTD_memcpy(DTableRank + 6, &DEltX2, sizeof(DEltX2));
      DTableRank += 8;
    }
    break;
  default:
    for (ptr = begin; ptr != end; ++ptr) {
      U64 const DEltX2 =
          HUF_buildDEltX2U64(ptr->symbol, nbBits, baseSeq, level);
      HUF_DEltX2 *const DTableRankEnd = DTableRank + length;
      for (; DTableRank != DTableRankEnd; DTableRank += 8) {
        ZSTD_memcpy(DTableRank + 0, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTableRank + 2, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTableRank + 4, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTableRank + 6, &DEltX2, sizeof(DEltX2));
      }
    }
    break;
  }
}

static void HUF_fillDTableX2Level2(HUF_DEltX2 *DTable, U32 targetLog,
                                   const U32 consumedBits, const U32 *rankVal,
                                   const int minWeight, const int maxWeight1,
                                   const sortedSymbol_t *sortedSymbols,
                                   U32 const *rankStart, U32 nbBitsBaseline,
                                   U16 baseSeq) {

  if (minWeight > 1) {
    U32 const length = 1U << ((targetLog - consumedBits) & 0x1F);
    U64 const DEltX2 = HUF_buildDEltX2U64(baseSeq, consumedBits, 0, 1);
    int const skipSize = rankVal[minWeight];
    assert(length > 1);
    assert((U32)skipSize < length);
    switch (length) {
    case 2:
      assert(skipSize == 1);
      ZSTD_memcpy(DTable, &DEltX2, sizeof(DEltX2));
      break;
    case 4:
      assert(skipSize <= 4);
      ZSTD_memcpy(DTable + 0, &DEltX2, sizeof(DEltX2));
      ZSTD_memcpy(DTable + 2, &DEltX2, sizeof(DEltX2));
      break;
    default: {
      int i;
      for (i = 0; i < skipSize; i += 8) {
        ZSTD_memcpy(DTable + i + 0, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTable + i + 2, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTable + i + 4, &DEltX2, sizeof(DEltX2));
        ZSTD_memcpy(DTable + i + 6, &DEltX2, sizeof(DEltX2));
      }
    }
    }
  }

  {
    int w;
    for (w = minWeight; w < maxWeight1; ++w) {
      int const begin = rankStart[w];
      int const end = rankStart[w + 1];
      U32 const nbBits = nbBitsBaseline - w;
      U32 const totalBits = nbBits + consumedBits;
      HUF_fillDTableX2ForWeight(DTable + rankVal[w], sortedSymbols + begin,
                                sortedSymbols + end, totalBits, targetLog,
                                baseSeq, 2);
    }
  }
}

static void HUF_fillDTableX2(HUF_DEltX2 *DTable, const U32 targetLog,
                             const sortedSymbol_t *sortedList,
                             const U32 *rankStart, rankValCol_t *rankValOrigin,
                             const U32 maxWeight, const U32 nbBitsBaseline) {
  U32 *const rankVal = rankValOrigin[0];
  const int scaleLog = nbBitsBaseline - targetLog;
  const U32 minBits = nbBitsBaseline - maxWeight;
  int w;
  int const wEnd = (int)maxWeight + 1;

  for (w = 1; w < wEnd; ++w) {
    int const begin = (int)rankStart[w];
    int const end = (int)rankStart[w + 1];
    U32 const nbBits = nbBitsBaseline - w;

    if (targetLog - nbBits >= minBits) {

      int start = rankVal[w];
      U32 const length = 1U << ((targetLog - nbBits) & 0x1F);
      int minWeight = nbBits + scaleLog;
      int s;
      if (minWeight < 1)
        minWeight = 1;

      for (s = begin; s != end; ++s) {
        HUF_fillDTableX2Level2(
            DTable + start, targetLog, nbBits, rankValOrigin[nbBits], minWeight,
            wEnd, sortedList, rankStart, nbBitsBaseline, sortedList[s].symbol);
        start += length;
      }
    } else {

      HUF_fillDTableX2ForWeight(DTable + rankVal[w], sortedList + begin,
                                sortedList + end, nbBits, targetLog, 0, 1);
    }
  }
}

typedef struct {
  rankValCol_t rankVal[HUF_TABLELOG_MAX];
  U32 rankStats[HUF_TABLELOG_MAX + 1];
  U32 rankStart0[HUF_TABLELOG_MAX + 3];
  sortedSymbol_t sortedSymbol[HUF_SYMBOLVALUE_MAX + 1];
  BYTE weightList[HUF_SYMBOLVALUE_MAX + 1];
  U32 calleeWksp[HUF_READ_STATS_WORKSPACE_SIZE_U32];
} HUF_ReadDTableX2_Workspace;

size_t HUF_readDTableX2_wksp(HUF_DTable *DTable, const void *src,
                             size_t srcSize, void *workSpace, size_t wkspSize,
                             int flags) {
  U32 tableLog, maxW, nbSymbols;
  DTableDesc dtd = HUF_getDTableDesc(DTable);
  U32 maxTableLog = dtd.maxTableLog;
  size_t iSize;
  void *dtPtr = DTable + 1;
  HUF_DEltX2 *const dt = (HUF_DEltX2 *)dtPtr;
  U32 *rankStart;

  HUF_ReadDTableX2_Workspace *const wksp =
      (HUF_ReadDTableX2_Workspace *)workSpace;

  if (sizeof(*wksp) > wkspSize)
    return ERROR(GENERIC);

  rankStart = wksp->rankStart0 + 1;
  ZSTD_memset(wksp->rankStats, 0, sizeof(wksp->rankStats));
  ZSTD_memset(wksp->rankStart0, 0, sizeof(wksp->rankStart0));

  DEBUG_STATIC_ASSERT(sizeof(HUF_DEltX2) == sizeof(HUF_DTable));
  if (maxTableLog > HUF_TABLELOG_MAX)
    return ERROR(tableLog_tooLarge);

  iSize =
      HUF_readStats_wksp(wksp->weightList, HUF_SYMBOLVALUE_MAX + 1,
                         wksp->rankStats, &nbSymbols, &tableLog, src, srcSize,
                         wksp->calleeWksp, sizeof(wksp->calleeWksp), flags);
  if (HUF_isError(iSize))
    return iSize;

  if (tableLog > maxTableLog)
    return ERROR(tableLog_tooLarge);
  if (tableLog <= HUF_DECODER_FAST_TABLELOG &&
      maxTableLog > HUF_DECODER_FAST_TABLELOG)
    maxTableLog = HUF_DECODER_FAST_TABLELOG;

  for (maxW = tableLog; wksp->rankStats[maxW] == 0; maxW--) {
  }

  {
    U32 w, nextRankStart = 0;
    for (w = 1; w < maxW + 1; w++) {
      U32 curr = nextRankStart;
      nextRankStart += wksp->rankStats[w];
      rankStart[w] = curr;
    }
    rankStart[0] = nextRankStart;
    rankStart[maxW + 1] = nextRankStart;
  }

  {
    U32 s;
    for (s = 0; s < nbSymbols; s++) {
      U32 const w = wksp->weightList[s];
      U32 const r = rankStart[w]++;
      wksp->sortedSymbol[r].symbol = (BYTE)s;
    }
    rankStart[0] = 0;
  }

  {
    U32 *const rankVal0 = wksp->rankVal[0];
    {
      int const rescale = (maxTableLog - tableLog) - 1;
      U32 nextRankVal = 0;
      U32 w;
      for (w = 1; w < maxW + 1; w++) {
        U32 curr = nextRankVal;
        nextRankVal += wksp->rankStats[w] << (w + rescale);
        rankVal0[w] = curr;
      }
    }
    {
      U32 const minBits = tableLog + 1 - maxW;
      U32 consumed;
      for (consumed = minBits; consumed < maxTableLog - minBits + 1;
           consumed++) {
        U32 *const rankValPtr = wksp->rankVal[consumed];
        U32 w;
        for (w = 1; w < maxW + 1; w++) {
          rankValPtr[w] = rankVal0[w] >> consumed;
        }
      }
    }
  }

  HUF_fillDTableX2(dt, maxTableLog, wksp->sortedSymbol, wksp->rankStart0,
                   wksp->rankVal, maxW, tableLog + 1);

  dtd.tableLog = (BYTE)maxTableLog;
  dtd.tableType = 1;
  ZSTD_memcpy(DTable, &dtd, sizeof(dtd));
  return iSize;
}

FORCE_INLINE_TEMPLATE U32 HUF_decodeSymbolX2(void *op, BIT_DStream_t *DStream,
                                             const HUF_DEltX2 *dt,
                                             const U32 dtLog) {
  size_t const val = BIT_lookBitsFast(DStream, dtLog);
  ZSTD_memcpy(op, &dt[val].sequence, 2);
  BIT_skipBits(DStream, dt[val].nbBits);
  return dt[val].length;
}

FORCE_INLINE_TEMPLATE U32 HUF_decodeLastSymbolX2(void *op,
                                                 BIT_DStream_t *DStream,
                                                 const HUF_DEltX2 *dt,
                                                 const U32 dtLog) {
  size_t const val = BIT_lookBitsFast(DStream, dtLog);
  ZSTD_memcpy(op, &dt[val].sequence, 1);
  if (dt[val].length == 1) {
    BIT_skipBits(DStream, dt[val].nbBits);
  } else {
    if (DStream->bitsConsumed < (sizeof(DStream->bitContainer) * 8)) {
      BIT_skipBits(DStream, dt[val].nbBits);
      if (DStream->bitsConsumed > (sizeof(DStream->bitContainer) * 8))

        DStream->bitsConsumed = (sizeof(DStream->bitContainer) * 8);
    }
  }
  return 1;
}

#define HUF_DECODE_SYMBOLX2_0(ptr, DStreamPtr)                                 \
  do {                                                                         \
    ptr += HUF_decodeSymbolX2(ptr, DStreamPtr, dt, dtLog);                     \
  } while (0)

#define HUF_DECODE_SYMBOLX2_1(ptr, DStreamPtr)                                 \
  do {                                                                         \
    if (MEM_64bits() || (HUF_TABLELOG_MAX <= 12))                              \
      ptr += HUF_decodeSymbolX2(ptr, DStreamPtr, dt, dtLog);                   \
  } while (0)

#define HUF_DECODE_SYMBOLX2_2(ptr, DStreamPtr)                                 \
  do {                                                                         \
    if (MEM_64bits())                                                          \
      ptr += HUF_decodeSymbolX2(ptr, DStreamPtr, dt, dtLog);                   \
  } while (0)

HINT_INLINE size_t HUF_decodeStreamX2(BYTE *p, BIT_DStream_t *bitDPtr,
                                      BYTE *const pEnd,
                                      const HUF_DEltX2 *const dt,
                                      const U32 dtLog) {
  BYTE *const pStart = p;

  if ((size_t)(pEnd - p) >= sizeof(bitDPtr->bitContainer)) {
    if (dtLog <= 11 && MEM_64bits()) {

      while ((BIT_reloadDStream(bitDPtr) == BIT_DStream_unfinished) &
             (p < pEnd - 9)) {
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
      }
    } else {

      while ((BIT_reloadDStream(bitDPtr) == BIT_DStream_unfinished) &
             (p < pEnd - (sizeof(bitDPtr->bitContainer) - 1))) {
        HUF_DECODE_SYMBOLX2_2(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_1(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_2(p, bitDPtr);
        HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
      }
    }
  } else {
    BIT_reloadDStream(bitDPtr);
  }

  if ((size_t)(pEnd - p) >= 2) {
    while ((BIT_reloadDStream(bitDPtr) == BIT_DStream_unfinished) &
           (p <= pEnd - 2))
      HUF_DECODE_SYMBOLX2_0(p, bitDPtr);

    while (p <= pEnd - 2)
      HUF_DECODE_SYMBOLX2_0(p, bitDPtr);
  }

  if (p < pEnd)
    p += HUF_decodeLastSymbolX2(p, bitDPtr, dt, dtLog);

  return p - pStart;
}

FORCE_INLINE_TEMPLATE size_t HUF_decompress1X2_usingDTable_internal_body(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable) {
  BIT_DStream_t bitD;

  CHECK_F(BIT_initDStream(&bitD, cSrc, cSrcSize));

  {
    BYTE *const ostart = (BYTE *)dst;
    BYTE *const oend = (BYTE *)ZSTD_maybeNullPtrAdd(ostart, (ptrdiff_t)dstSize);
    const void *const dtPtr = DTable + 1;
    const HUF_DEltX2 *const dt = (const HUF_DEltX2 *)dtPtr;
    DTableDesc const dtd = HUF_getDTableDesc(DTable);
    HUF_decodeStreamX2(ostart, &bitD, oend, dt, dtd.tableLog);
  }

  if (!BIT_endOfDStream(&bitD))
    return ERROR(corruption_detected);

  return dstSize;
}

FORCE_INLINE_TEMPLATE size_t HUF_decompress4X2_usingDTable_internal_body(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable) {
  if (cSrcSize < 10)
    return ERROR(corruption_detected);

  if (dstSize < 6)
    return ERROR(corruption_detected);

  {
    const BYTE *const istart = (const BYTE *)cSrc;
    BYTE *const ostart = (BYTE *)dst;
    BYTE *const oend = ostart + dstSize;
    BYTE *const olimit = oend - (sizeof(size_t) - 1);
    const void *const dtPtr = DTable + 1;
    const HUF_DEltX2 *const dt = (const HUF_DEltX2 *)dtPtr;

    BIT_DStream_t bitD1;
    BIT_DStream_t bitD2;
    BIT_DStream_t bitD3;
    BIT_DStream_t bitD4;
    size_t const length1 = MEM_readLE16(istart);
    size_t const length2 = MEM_readLE16(istart + 2);
    size_t const length3 = MEM_readLE16(istart + 4);
    size_t const length4 = cSrcSize - (length1 + length2 + length3 + 6);
    const BYTE *const istart1 = istart + 6;
    const BYTE *const istart2 = istart1 + length1;
    const BYTE *const istart3 = istart2 + length2;
    const BYTE *const istart4 = istart3 + length3;
    size_t const segmentSize = (dstSize + 3) / 4;
    BYTE *const opStart2 = ostart + segmentSize;
    BYTE *const opStart3 = opStart2 + segmentSize;
    BYTE *const opStart4 = opStart3 + segmentSize;
    BYTE *op1 = ostart;
    BYTE *op2 = opStart2;
    BYTE *op3 = opStart3;
    BYTE *op4 = opStart4;
    U32 endSignal = 1;
    DTableDesc const dtd = HUF_getDTableDesc(DTable);
    U32 const dtLog = dtd.tableLog;

    if (length4 > cSrcSize)
      return ERROR(corruption_detected);
    if (opStart4 > oend)
      return ERROR(corruption_detected);
    assert(dstSize >= 6);
    CHECK_F(BIT_initDStream(&bitD1, istart1, length1));
    CHECK_F(BIT_initDStream(&bitD2, istart2, length2));
    CHECK_F(BIT_initDStream(&bitD3, istart3, length3));
    CHECK_F(BIT_initDStream(&bitD4, istart4, length4));

    if ((size_t)(oend - op4) >= sizeof(size_t)) {
      for (; (endSignal) & (op4 < olimit);) {
#if defined(__clang__) && (defined(__x86_64__) || defined(__i386__))
        HUF_DECODE_SYMBOLX2_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_1(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_0(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_1(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_0(op2, &bitD2);
        endSignal &= BIT_reloadDStreamFast(&bitD1) == BIT_DStream_unfinished;
        endSignal &= BIT_reloadDStreamFast(&bitD2) == BIT_DStream_unfinished;
        HUF_DECODE_SYMBOLX2_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_1(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_0(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_1(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_0(op4, &bitD4);
        endSignal &= BIT_reloadDStreamFast(&bitD3) == BIT_DStream_unfinished;
        endSignal &= BIT_reloadDStreamFast(&bitD4) == BIT_DStream_unfinished;
#else
        HUF_DECODE_SYMBOLX2_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_1(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_1(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_1(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_1(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_2(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_2(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_2(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_2(op4, &bitD4);
        HUF_DECODE_SYMBOLX2_0(op1, &bitD1);
        HUF_DECODE_SYMBOLX2_0(op2, &bitD2);
        HUF_DECODE_SYMBOLX2_0(op3, &bitD3);
        HUF_DECODE_SYMBOLX2_0(op4, &bitD4);
        endSignal = (U32)LIKELY(
            (U32)(BIT_reloadDStreamFast(&bitD1) == BIT_DStream_unfinished) &
            (BIT_reloadDStreamFast(&bitD2) == BIT_DStream_unfinished) &
            (BIT_reloadDStreamFast(&bitD3) == BIT_DStream_unfinished) &
            (BIT_reloadDStreamFast(&bitD4) == BIT_DStream_unfinished));
#endif
      }
    }

    if (op1 > opStart2)
      return ERROR(corruption_detected);
    if (op2 > opStart3)
      return ERROR(corruption_detected);
    if (op3 > opStart4)
      return ERROR(corruption_detected);

    HUF_decodeStreamX2(op1, &bitD1, opStart2, dt, dtLog);
    HUF_decodeStreamX2(op2, &bitD2, opStart3, dt, dtLog);
    HUF_decodeStreamX2(op3, &bitD3, opStart4, dt, dtLog);
    HUF_decodeStreamX2(op4, &bitD4, oend, dt, dtLog);

    {
      U32 const endCheck = BIT_endOfDStream(&bitD1) & BIT_endOfDStream(&bitD2) &
                           BIT_endOfDStream(&bitD3) & BIT_endOfDStream(&bitD4);
      if (!endCheck)
        return ERROR(corruption_detected);
    }

    return dstSize;
  }
}

#if HUF_NEED_BMI2_FUNCTION
static BMI2_TARGET_ATTRIBUTE size_t HUF_decompress4X2_usingDTable_internal_bmi2(
    void *dst, size_t dstSize, void const *cSrc, size_t cSrcSize,
    HUF_DTable const *DTable) {
  return HUF_decompress4X2_usingDTable_internal_body(dst, dstSize, cSrc,
                                                     cSrcSize, DTable);
}
#endif

static size_t HUF_decompress4X2_usingDTable_internal_default(
    void *dst, size_t dstSize, void const *cSrc, size_t cSrcSize,
    HUF_DTable const *DTable) {
  return HUF_decompress4X2_usingDTable_internal_body(dst, dstSize, cSrc,
                                                     cSrcSize, DTable);
}

#if ZSTD_ENABLE_ASM_X86_64_BMI2

HUF_ASM_DECL void HUF_decompress4X2_usingDTable_internal_fast_asm_loop(
    HUF_DecompressFastArgs *args) ZSTDLIB_HIDDEN;

#endif

static HUF_FAST_BMI2_ATTRS void
HUF_decompress4X2_usingDTable_internal_fast_c_loop(
    HUF_DecompressFastArgs *args) {
  U64 bits[4];
  BYTE const *ip[4];
  BYTE *op[4];
  BYTE *oend[4];
  HUF_DEltX2 const *const dtable = (HUF_DEltX2 const *)args->dt;
  BYTE const *const ilowest = args->ilowest;

  ZSTD_memcpy(&bits, &args->bits, sizeof(bits));
  ZSTD_memcpy((void *)(&ip), &args->ip, sizeof(ip));
  ZSTD_memcpy(&op, &args->op, sizeof(op));

  oend[0] = op[1];
  oend[1] = op[2];
  oend[2] = op[3];
  oend[3] = args->oend;

  assert(MEM_isLittleEndian());
  assert(!MEM_32bits());

  for (;;) {
    BYTE *olimit;
    int stream;

#ifndef NDEBUG
    for (stream = 0; stream < 4; ++stream) {
      assert(op[stream] <= oend[stream]);
      assert(ip[stream] >= ilowest);
    }
#endif

    {

      size_t iters = (size_t)(ip[0] - ilowest) / 7;

      for (stream = 0; stream < 4; ++stream) {
        size_t const oiters = (size_t)(oend[stream] - op[stream]) / 10;
        iters = MIN(iters, oiters);
      }

      olimit = op[3] + (iters * 5);

      if (op[3] == olimit)
        break;

      for (stream = 1; stream < 4; ++stream) {
        if (ip[stream] < ip[stream - 1])
          goto _out;
      }
    }

#ifndef NDEBUG
    for (stream = 1; stream < 4; ++stream) {
      assert(ip[stream] >= ip[stream - 1]);
    }
#endif

#define HUF_4X2_DECODE_SYMBOL(_stream, _decode3)                               \
  do {                                                                         \
    if ((_decode3) || (_stream) != 3) {                                        \
      U64 const index = bits[(_stream)] >> 53;                                 \
      size_t const entry = MEM_readLE32(&dtable[index]);                       \
      MEM_write16(op[(_stream)], (U16)entry);                                  \
      bits[(_stream)] <<= (entry >> 16) & 0x3F;                                \
      op[(_stream)] += entry >> 24;                                            \
    }                                                                          \
  } while (0)

#define HUF_5X2_RELOAD_STREAM(_stream, _decode3)                               \
  do {                                                                         \
    if (_decode3)                                                              \
      HUF_4X2_DECODE_SYMBOL(3, 1);                                             \
    {                                                                          \
      U64 const ctz = ZSTD_countTrailingZeros64(bits[(_stream)]);              \
      U64 const nbBits = ctz & 7;                                              \
      U64 const nbBytes = ctz >> 3;                                            \
      ip[(_stream)] -= nbBytes;                                                \
      bits[(_stream)] = MEM_read64(ip[(_stream)]) | 1;                         \
      bits[(_stream)] <<= nbBits;                                              \
    }                                                                          \
  } while (0)

#if defined(__aarch64__)
#define HUF_4X2_4WAY 1
#else
#define HUF_4X2_4WAY 0
#endif
#define HUF_4X2_3WAY !HUF_4X2_4WAY

    do {

      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X2_DECODE_SYMBOL, HUF_4X2_4WAY);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X2_DECODE_SYMBOL, HUF_4X2_4WAY);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X2_DECODE_SYMBOL, HUF_4X2_4WAY);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X2_DECODE_SYMBOL, HUF_4X2_4WAY);
      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_4X2_DECODE_SYMBOL, HUF_4X2_4WAY);

      HUF_4X2_DECODE_SYMBOL(3, HUF_4X2_3WAY);

      HUF_4X_FOR_EACH_STREAM_WITH_VAR(HUF_5X2_RELOAD_STREAM, HUF_4X2_3WAY);
    } while (op[3] < olimit);
  }

#undef HUF_4X2_DECODE_SYMBOL
#undef HUF_5X2_RELOAD_STREAM

_out:

  ZSTD_memcpy(&args->bits, &bits, sizeof(bits));
  ZSTD_memcpy((void *)(&args->ip), &ip, sizeof(ip));
  ZSTD_memcpy(&args->op, &op, sizeof(op));
}

static HUF_FAST_BMI2_ATTRS size_t HUF_decompress4X2_usingDTable_internal_fast(
    void *dst, size_t dstSize, const void *cSrc, size_t cSrcSize,
    const HUF_DTable *DTable, HUF_DecompressFastLoopFn loopFn) {
  void const *dt = DTable + 1;
  const BYTE *const ilowest = (const BYTE *)cSrc;
  BYTE *const oend = (BYTE *)ZSTD_maybeNullPtrAdd(dst, (ptrdiff_t)dstSize);
  HUF_DecompressFastArgs args;
  {
    size_t const ret = HUF_DecompressFastArgs_init(&args, dst, dstSize, cSrc,
                                                   cSrcSize, DTable);
    FORWARD_IF_ERROR(ret, "Failed to init asm args");
    if (ret == 0)
      return 0;
  }

  assert(args.ip[0] >= args.ilowest);
  loopFn(&args);

  assert(args.ip[0] >= ilowest);
  assert(args.ip[1] >= ilowest);
  assert(args.ip[2] >= ilowest);
  assert(args.ip[3] >= ilowest);
  assert(args.op[3] <= oend);

  assert(ilowest == args.ilowest);
  assert(ilowest + 6 == args.iend[0]);
  (void)ilowest;

  {
    size_t const segmentSize = (dstSize + 3) / 4;
    BYTE *segmentEnd = (BYTE *)dst;
    int i;
    for (i = 0; i < 4; ++i) {
      BIT_DStream_t bit;
      if (segmentSize <= (size_t)(oend - segmentEnd))
        segmentEnd += segmentSize;
      else
        segmentEnd = oend;
      FORWARD_IF_ERROR(HUF_initRemainingDStream(&bit, &args, i, segmentEnd),
                       "corruption");
      args.op[i] +=
          HUF_decodeStreamX2(args.op[i], &bit, segmentEnd,
                             (HUF_DEltX2 const *)dt, HUF_DECODER_FAST_TABLELOG);
      if (args.op[i] != segmentEnd)
        return ERROR(corruption_detected);
    }
  }

  return dstSize;
}

static size_t HUF_decompress4X2_usingDTable_internal(void *dst, size_t dstSize,
                                                     void const *cSrc,
                                                     size_t cSrcSize,
                                                     HUF_DTable const *DTable,
                                                     int flags) {
  HUF_DecompressUsingDTableFn fallbackFn =
      HUF_decompress4X2_usingDTable_internal_default;
  HUF_DecompressFastLoopFn loopFn =
      HUF_decompress4X2_usingDTable_internal_fast_c_loop;

#if DYNAMIC_BMI2
  if (flags & HUF_flags_bmi2) {
    fallbackFn = HUF_decompress4X2_usingDTable_internal_bmi2;
#if ZSTD_ENABLE_ASM_X86_64_BMI2
    if (!(flags & HUF_flags_disableAsm)) {
      loopFn = HUF_decompress4X2_usingDTable_internal_fast_asm_loop;
    }
#endif
  } else {
    return fallbackFn(dst, dstSize, cSrc, cSrcSize, DTable);
  }
#endif

#if ZSTD_ENABLE_ASM_X86_64_BMI2 && defined(__BMI2__)
  if (!(flags & HUF_flags_disableAsm)) {
    loopFn = HUF_decompress4X2_usingDTable_internal_fast_asm_loop;
  }
#endif

  if (HUF_ENABLE_FAST_DECODE && !(flags & HUF_flags_disableFast)) {
    size_t const ret = HUF_decompress4X2_usingDTable_internal_fast(
        dst, dstSize, cSrc, cSrcSize, DTable, loopFn);
    if (ret != 0)
      return ret;
  }
  return fallbackFn(dst, dstSize, cSrc, cSrcSize, DTable);
}

HUF_DGEN(HUF_decompress1X2_usingDTable_internal)

size_t HUF_decompress1X2_DCtx_wksp(HUF_DTable *DCtx, void *dst, size_t dstSize,
                                   const void *cSrc, size_t cSrcSize,
                                   void *workSpace, size_t wkspSize,
                                   int flags) {
  const BYTE *ip = (const BYTE *)cSrc;

  size_t const hSize =
      HUF_readDTableX2_wksp(DCtx, cSrc, cSrcSize, workSpace, wkspSize, flags);
  if (HUF_isError(hSize))
    return hSize;
  if (hSize >= cSrcSize)
    return ERROR(srcSize_wrong);
  ip += hSize;
  cSrcSize -= hSize;

  return HUF_decompress1X2_usingDTable_internal(dst, dstSize, ip, cSrcSize,
                                                DCtx, flags);
}

static size_t HUF_decompress4X2_DCtx_wksp(HUF_DTable *dctx, void *dst,
                                          size_t dstSize, const void *cSrc,
                                          size_t cSrcSize, void *workSpace,
                                          size_t wkspSize, int flags) {
  const BYTE *ip = (const BYTE *)cSrc;

  size_t hSize =
      HUF_readDTableX2_wksp(dctx, cSrc, cSrcSize, workSpace, wkspSize, flags);
  if (HUF_isError(hSize))
    return hSize;
  if (hSize >= cSrcSize)
    return ERROR(srcSize_wrong);
  ip += hSize;
  cSrcSize -= hSize;

  return HUF_decompress4X2_usingDTable_internal(dst, dstSize, ip, cSrcSize,
                                                dctx, flags);
}

#endif

#if !defined(HUF_FORCE_DECOMPRESS_X1) && !defined(HUF_FORCE_DECOMPRESS_X2)
typedef struct {
  U32 tableTime;
  U32 decode256Time;
} algo_time_t;
static const algo_time_t algoTime[16][2] = {

    {{0, 0}, {1, 1}},           {{0, 0}, {1, 1}},
    {{150, 216}, {381, 119}},   {{170, 205}, {514, 112}},
    {{177, 199}, {539, 110}},   {{197, 194}, {644, 107}},
    {{221, 192}, {735, 107}},   {{256, 189}, {881, 106}},
    {{359, 188}, {1167, 109}},  {{582, 187}, {1570, 114}},
    {{688, 187}, {1712, 122}},  {{825, 186}, {1965, 136}},
    {{976, 185}, {2131, 150}},  {{1180, 186}, {2070, 175}},
    {{1377, 185}, {1731, 202}}, {{1412, 185}, {1695, 202}},
};
#endif

U32 HUF_selectDecoder(size_t dstSize, size_t cSrcSize) {
  assert(dstSize > 0);
  assert(dstSize <= 128 * 1024);
#if defined(HUF_FORCE_DECOMPRESS_X1)
  (void)dstSize;
  (void)cSrcSize;
  return 0;
#elif defined(HUF_FORCE_DECOMPRESS_X2)
  (void)dstSize;
  (void)cSrcSize;
  return 1;
#else

  {
    U32 const Q = (cSrcSize >= dstSize) ? 15 : (U32)(cSrcSize * 16 / dstSize);
    U32 const D256 = (U32)(dstSize >> 8);
    U32 const DTime0 =
        algoTime[Q][0].tableTime + (algoTime[Q][0].decode256Time * D256);
    U32 DTime1 =
        algoTime[Q][1].tableTime + (algoTime[Q][1].decode256Time * D256);
    DTime1 += DTime1 >> 5;

    return DTime1 < DTime0;
  }
#endif
}

size_t HUF_decompress1X_DCtx_wksp(HUF_DTable *dctx, void *dst, size_t dstSize,
                                  const void *cSrc, size_t cSrcSize,
                                  void *workSpace, size_t wkspSize, int flags) {

  if (dstSize == 0)
    return ERROR(dstSize_tooSmall);
  if (cSrcSize > dstSize)
    return ERROR(corruption_detected);
  if (cSrcSize == dstSize) {
    ZSTD_memcpy(dst, cSrc, dstSize);
    return dstSize;
  }
  if (cSrcSize == 1) {
    ZSTD_memset(dst, *(const BYTE *)cSrc, dstSize);
    return dstSize;
  }

  {
    U32 const algoNb = HUF_selectDecoder(dstSize, cSrcSize);
#if defined(HUF_FORCE_DECOMPRESS_X1)
    (void)algoNb;
    assert(algoNb == 0);
    return HUF_decompress1X1_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                       workSpace, wkspSize, flags);
#elif defined(HUF_FORCE_DECOMPRESS_X2)
    (void)algoNb;
    assert(algoNb == 1);
    return HUF_decompress1X2_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                       workSpace, wkspSize, flags);
#else
    return algoNb
               ? HUF_decompress1X2_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                             workSpace, wkspSize, flags)
               : HUF_decompress1X1_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                             workSpace, wkspSize, flags);
#endif
  }
}

size_t HUF_decompress1X_usingDTable(void *dst, size_t maxDstSize,
                                    const void *cSrc, size_t cSrcSize,
                                    const HUF_DTable *DTable, int flags) {
  DTableDesc const dtd = HUF_getDTableDesc(DTable);
#if defined(HUF_FORCE_DECOMPRESS_X1)
  (void)dtd;
  assert(dtd.tableType == 0);
  return HUF_decompress1X1_usingDTable_internal(dst, maxDstSize, cSrc, cSrcSize,
                                                DTable, flags);
#elif defined(HUF_FORCE_DECOMPRESS_X2)
  (void)dtd;
  assert(dtd.tableType == 1);
  return HUF_decompress1X2_usingDTable_internal(dst, maxDstSize, cSrc, cSrcSize,
                                                DTable, flags);
#else
  return dtd.tableType
             ? HUF_decompress1X2_usingDTable_internal(dst, maxDstSize, cSrc,
                                                      cSrcSize, DTable, flags)
             : HUF_decompress1X1_usingDTable_internal(dst, maxDstSize, cSrc,
                                                      cSrcSize, DTable, flags);
#endif
}

#ifndef HUF_FORCE_DECOMPRESS_X2
size_t HUF_decompress1X1_DCtx_wksp(HUF_DTable *dctx, void *dst, size_t dstSize,
                                   const void *cSrc, size_t cSrcSize,
                                   void *workSpace, size_t wkspSize,
                                   int flags) {
  const BYTE *ip = (const BYTE *)cSrc;

  size_t const hSize =
      HUF_readDTableX1_wksp(dctx, cSrc, cSrcSize, workSpace, wkspSize, flags);
  if (HUF_isError(hSize))
    return hSize;
  if (hSize >= cSrcSize)
    return ERROR(srcSize_wrong);
  ip += hSize;
  cSrcSize -= hSize;

  return HUF_decompress1X1_usingDTable_internal(dst, dstSize, ip, cSrcSize,
                                                dctx, flags);
}
#endif

size_t HUF_decompress4X_usingDTable(void *dst, size_t maxDstSize,
                                    const void *cSrc, size_t cSrcSize,
                                    const HUF_DTable *DTable, int flags) {
  DTableDesc const dtd = HUF_getDTableDesc(DTable);
#if defined(HUF_FORCE_DECOMPRESS_X1)
  (void)dtd;
  assert(dtd.tableType == 0);
  return HUF_decompress4X1_usingDTable_internal(dst, maxDstSize, cSrc, cSrcSize,
                                                DTable, flags);
#elif defined(HUF_FORCE_DECOMPRESS_X2)
  (void)dtd;
  assert(dtd.tableType == 1);
  return HUF_decompress4X2_usingDTable_internal(dst, maxDstSize, cSrc, cSrcSize,
                                                DTable, flags);
#else
  return dtd.tableType
             ? HUF_decompress4X2_usingDTable_internal(dst, maxDstSize, cSrc,
                                                      cSrcSize, DTable, flags)
             : HUF_decompress4X1_usingDTable_internal(dst, maxDstSize, cSrc,
                                                      cSrcSize, DTable, flags);
#endif
}

size_t HUF_decompress4X_hufOnly_wksp(HUF_DTable *dctx, void *dst,
                                     size_t dstSize, const void *cSrc,
                                     size_t cSrcSize, void *workSpace,
                                     size_t wkspSize, int flags) {

  if (dstSize == 0)
    return ERROR(dstSize_tooSmall);
  if (cSrcSize == 0)
    return ERROR(corruption_detected);

  {
    U32 const algoNb = HUF_selectDecoder(dstSize, cSrcSize);
#if defined(HUF_FORCE_DECOMPRESS_X1)
    (void)algoNb;
    assert(algoNb == 0);
    return HUF_decompress4X1_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                       workSpace, wkspSize, flags);
#elif defined(HUF_FORCE_DECOMPRESS_X2)
    (void)algoNb;
    assert(algoNb == 1);
    return HUF_decompress4X2_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                       workSpace, wkspSize, flags);
#else
    return algoNb
               ? HUF_decompress4X2_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                             workSpace, wkspSize, flags)
               : HUF_decompress4X1_DCtx_wksp(dctx, dst, dstSize, cSrc, cSrcSize,
                                             workSpace, wkspSize, flags);
#endif
  }
}
