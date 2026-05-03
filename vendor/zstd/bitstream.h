/* ******************************************************************
 * bitstream
 * Part of FSE library
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * You can contact the author at :
 * - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 ****************************************************************** */
#ifndef BITSTREAM_H_MODULE
#define BITSTREAM_H_MODULE

#include "bits.h"
#include "compiler.h"
#include "debug.h"
#include "error_private.h"
#include "mem.h"
#include <string.h>

#ifndef ZSTD_NO_INTRINSICS
#if (defined(__BMI__) || defined(__BMI2__)) && defined(__GNUC__)
#include <immintrin.h>
#elif defined(__ICCARM__)
#include <intrinsics.h>
#endif
#endif

#define STREAM_ACCUMULATOR_MIN_32 25
#define STREAM_ACCUMULATOR_MIN_64 57
#define STREAM_ACCUMULATOR_MIN                                                 \
  ((U32)(MEM_32bits() ? STREAM_ACCUMULATOR_MIN_32 : STREAM_ACCUMULATOR_MIN_64))

typedef size_t BitContainerType;

typedef struct {
  BitContainerType bitContainer;
  unsigned bitPos;
  char *startPtr;
  char *ptr;
  char *endPtr;
} BIT_CStream_t;

MEM_STATIC size_t BIT_initCStream(BIT_CStream_t *bitC, void *dstBuffer,
                                  size_t dstCapacity);
MEM_STATIC void BIT_addBits(BIT_CStream_t *bitC, BitContainerType value,
                            unsigned nbBits);
MEM_STATIC void BIT_flushBits(BIT_CStream_t *bitC);
MEM_STATIC size_t BIT_closeCStream(BIT_CStream_t *bitC);

typedef struct {
  BitContainerType bitContainer;
  unsigned bitsConsumed;
  const char *ptr;
  const char *start;
  const char *limitPtr;
} BIT_DStream_t;

typedef enum {
  BIT_DStream_unfinished = 0,
  BIT_DStream_endOfBuffer = 1,
  BIT_DStream_completed = 2,
  BIT_DStream_overflow = 3
} BIT_DStream_status;

MEM_STATIC size_t BIT_initDStream(BIT_DStream_t *bitD, const void *srcBuffer,
                                  size_t srcSize);
FORCE_INLINE_TEMPLATE BitContainerType BIT_readBits(BIT_DStream_t *bitD,
                                                    unsigned nbBits);
FORCE_INLINE_TEMPLATE BIT_DStream_status BIT_reloadDStream(BIT_DStream_t *bitD);
MEM_STATIC unsigned BIT_endOfDStream(const BIT_DStream_t *bitD);

MEM_STATIC void BIT_addBitsFast(BIT_CStream_t *bitC, BitContainerType value,
                                unsigned nbBits);

MEM_STATIC void BIT_flushBitsFast(BIT_CStream_t *bitC);

MEM_STATIC size_t BIT_readBitsFast(BIT_DStream_t *bitD, unsigned nbBits);

static const unsigned BIT_mask[] = {
    0,          1,         3,         7,         0xF,       0x1F,
    0x3F,       0x7F,      0xFF,      0x1FF,     0x3FF,     0x7FF,
    0xFFF,      0x1FFF,    0x3FFF,    0x7FFF,    0xFFFF,    0x1FFFF,
    0x3FFFF,    0x7FFFF,   0xFFFFF,   0x1FFFFF,  0x3FFFFF,  0x7FFFFF,
    0xFFFFFF,   0x1FFFFFF, 0x3FFFFFF, 0x7FFFFFF, 0xFFFFFFF, 0x1FFFFFFF,
    0x3FFFFFFF, 0x7FFFFFFF};
#define BIT_MASK_SIZE (sizeof(BIT_mask) / sizeof(BIT_mask[0]))

MEM_STATIC size_t BIT_initCStream(BIT_CStream_t *bitC, void *startPtr,
                                  size_t dstCapacity) {
  bitC->bitContainer = 0;
  bitC->bitPos = 0;
  bitC->startPtr = (char *)startPtr;
  bitC->ptr = bitC->startPtr;
  bitC->endPtr = bitC->startPtr + dstCapacity - sizeof(bitC->bitContainer);
  if (dstCapacity <= sizeof(bitC->bitContainer))
    return ERROR(dstSize_tooSmall);
  return 0;
}

FORCE_INLINE_TEMPLATE BitContainerType
BIT_getLowerBits(BitContainerType bitContainer, U32 const nbBits) {
#if STATIC_BMI2 && !defined(ZSTD_NO_INTRINSICS)
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__ILP32__)
  return _bzhi_u64(bitContainer, nbBits);
#else
  DEBUG_STATIC_ASSERT(sizeof(bitContainer) == sizeof(U32));
  return _bzhi_u32(bitContainer, nbBits);
#endif
#else
  assert(nbBits < BIT_MASK_SIZE);
  return bitContainer & BIT_mask[nbBits];
#endif
}

MEM_STATIC void BIT_addBits(BIT_CStream_t *bitC, BitContainerType value,
                            unsigned nbBits) {
  DEBUG_STATIC_ASSERT(BIT_MASK_SIZE == 32);
  assert(nbBits < BIT_MASK_SIZE);
  assert(nbBits + bitC->bitPos < sizeof(bitC->bitContainer) * 8);
  bitC->bitContainer |= BIT_getLowerBits(value, nbBits) << bitC->bitPos;
  bitC->bitPos += nbBits;
}

MEM_STATIC void BIT_addBitsFast(BIT_CStream_t *bitC, BitContainerType value,
                                unsigned nbBits) {
  assert((value >> nbBits) == 0);
  assert(nbBits + bitC->bitPos < sizeof(bitC->bitContainer) * 8);
  bitC->bitContainer |= value << bitC->bitPos;
  bitC->bitPos += nbBits;
}

MEM_STATIC void BIT_flushBitsFast(BIT_CStream_t *bitC) {
  size_t const nbBytes = bitC->bitPos >> 3;
  assert(bitC->bitPos < sizeof(bitC->bitContainer) * 8);
  assert(bitC->ptr <= bitC->endPtr);
  MEM_writeLEST(bitC->ptr, bitC->bitContainer);
  bitC->ptr += nbBytes;
  bitC->bitPos &= 7;
  bitC->bitContainer >>= nbBytes * 8;
}

MEM_STATIC void BIT_flushBits(BIT_CStream_t *bitC) {
  size_t const nbBytes = bitC->bitPos >> 3;
  assert(bitC->bitPos < sizeof(bitC->bitContainer) * 8);
  assert(bitC->ptr <= bitC->endPtr);
  MEM_writeLEST(bitC->ptr, bitC->bitContainer);
  bitC->ptr += nbBytes;
  if (bitC->ptr > bitC->endPtr)
    bitC->ptr = bitC->endPtr;
  bitC->bitPos &= 7;
  bitC->bitContainer >>= nbBytes * 8;
}

MEM_STATIC size_t BIT_closeCStream(BIT_CStream_t *bitC) {
  BIT_addBitsFast(bitC, 1, 1);
  BIT_flushBits(bitC);
  if (bitC->ptr >= bitC->endPtr)
    return 0;
  return (size_t)(bitC->ptr - bitC->startPtr) + (bitC->bitPos > 0);
}

MEM_STATIC size_t BIT_initDStream(BIT_DStream_t *bitD, const void *srcBuffer,
                                  size_t srcSize) {
  if (srcSize < 1) {
    ZSTD_memset(bitD, 0, sizeof(*bitD));
    return ERROR(srcSize_wrong);
  }

  bitD->start = (const char *)srcBuffer;
  bitD->limitPtr = bitD->start + sizeof(bitD->bitContainer);

  if (srcSize >= sizeof(bitD->bitContainer)) {
    bitD->ptr = (const char *)srcBuffer + srcSize - sizeof(bitD->bitContainer);
    bitD->bitContainer = MEM_readLEST(bitD->ptr);
    {
      BYTE const lastByte = ((const BYTE *)srcBuffer)[srcSize - 1];
      bitD->bitsConsumed = lastByte ? 8 - ZSTD_highbit32(lastByte) : 0;
      if (lastByte == 0)
        return ERROR(GENERIC);
    }
  } else {
    bitD->ptr = bitD->start;
    bitD->bitContainer = *(const BYTE *)(bitD->start);
    switch (srcSize) {
    case 7:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[6])
                            << (sizeof(bitD->bitContainer) * 8 - 16);
      ZSTD_FALLTHROUGH;

    case 6:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[5])
                            << (sizeof(bitD->bitContainer) * 8 - 24);
      ZSTD_FALLTHROUGH;

    case 5:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[4])
                            << (sizeof(bitD->bitContainer) * 8 - 32);
      ZSTD_FALLTHROUGH;

    case 4:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[3])
                            << 24;
      ZSTD_FALLTHROUGH;

    case 3:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[2])
                            << 16;
      ZSTD_FALLTHROUGH;

    case 2:
      bitD->bitContainer += (BitContainerType)(((const BYTE *)(srcBuffer))[1])
                            << 8;
      ZSTD_FALLTHROUGH;

    default:
      break;
    }
    {
      BYTE const lastByte = ((const BYTE *)srcBuffer)[srcSize - 1];
      bitD->bitsConsumed = lastByte ? 8 - ZSTD_highbit32(lastByte) : 0;
      if (lastByte == 0)
        return ERROR(corruption_detected);
    }
    bitD->bitsConsumed += (U32)(sizeof(bitD->bitContainer) - srcSize) * 8;
  }

  return srcSize;
}

FORCE_INLINE_TEMPLATE BitContainerType
BIT_getUpperBits(BitContainerType bitContainer, U32 const start) {
  return bitContainer >> start;
}

FORCE_INLINE_TEMPLATE BitContainerType BIT_getMiddleBits(
    BitContainerType bitContainer, U32 const start, U32 const nbBits) {
  U32 const regMask = sizeof(bitContainer) * 8 - 1;

  assert(nbBits < BIT_MASK_SIZE);

#if defined(__x86_64__) || defined(_M_X64)
  return (bitContainer >> (start & regMask)) & ((((U64)1) << nbBits) - 1);
#else
  return (bitContainer >> (start & regMask)) & BIT_mask[nbBits];
#endif
}

FORCE_INLINE_TEMPLATE BitContainerType BIT_lookBits(const BIT_DStream_t *bitD,
                                                    U32 nbBits) {

#if 1

  return BIT_getMiddleBits(
      bitD->bitContainer,
      (sizeof(bitD->bitContainer) * 8) - bitD->bitsConsumed - nbBits, nbBits);
#else

  U32 const regMask = sizeof(bitD->bitContainer) * 8 - 1;
  return ((bitD->bitContainer << (bitD->bitsConsumed & regMask)) >> 1) >>
         ((regMask - nbBits) & regMask);
#endif
}

MEM_STATIC BitContainerType BIT_lookBitsFast(const BIT_DStream_t *bitD,
                                             U32 nbBits) {
  U32 const regMask = sizeof(bitD->bitContainer) * 8 - 1;
  assert(nbBits >= 1);
  return (bitD->bitContainer << (bitD->bitsConsumed & regMask)) >>
         (((regMask + 1) - nbBits) & regMask);
}

FORCE_INLINE_TEMPLATE void BIT_skipBits(BIT_DStream_t *bitD, U32 nbBits) {
  bitD->bitsConsumed += nbBits;
}

FORCE_INLINE_TEMPLATE BitContainerType BIT_readBits(BIT_DStream_t *bitD,
                                                    unsigned nbBits) {
  BitContainerType const value = BIT_lookBits(bitD, nbBits);
  BIT_skipBits(bitD, nbBits);
  return value;
}

MEM_STATIC BitContainerType BIT_readBitsFast(BIT_DStream_t *bitD,
                                             unsigned nbBits) {
  BitContainerType const value = BIT_lookBitsFast(bitD, nbBits);
  assert(nbBits >= 1);
  BIT_skipBits(bitD, nbBits);
  return value;
}

MEM_STATIC BIT_DStream_status BIT_reloadDStream_internal(BIT_DStream_t *bitD) {
  assert(bitD->bitsConsumed <= sizeof(bitD->bitContainer) * 8);
  bitD->ptr -= bitD->bitsConsumed >> 3;
  assert(bitD->ptr >= bitD->start);
  bitD->bitsConsumed &= 7;
  bitD->bitContainer = MEM_readLEST(bitD->ptr);
  return BIT_DStream_unfinished;
}

MEM_STATIC BIT_DStream_status BIT_reloadDStreamFast(BIT_DStream_t *bitD) {
  if (UNLIKELY(bitD->ptr < bitD->limitPtr))
    return BIT_DStream_overflow;
  return BIT_reloadDStream_internal(bitD);
}

FORCE_INLINE_TEMPLATE BIT_DStream_status
BIT_reloadDStream(BIT_DStream_t *bitD) {

  if (UNLIKELY(bitD->bitsConsumed > (sizeof(bitD->bitContainer) * 8))) {
    static const BitContainerType zeroFilled = 0;
    bitD->ptr = (const char *)&zeroFilled;

    return BIT_DStream_overflow;
  }

  assert(bitD->ptr >= bitD->start);

  if (bitD->ptr >= bitD->limitPtr) {
    return BIT_reloadDStream_internal(bitD);
  }
  if (bitD->ptr == bitD->start) {

    if (bitD->bitsConsumed < sizeof(bitD->bitContainer) * 8)
      return BIT_DStream_endOfBuffer;
    return BIT_DStream_completed;
  }

  {
    U32 nbBytes = bitD->bitsConsumed >> 3;
    BIT_DStream_status result = BIT_DStream_unfinished;
    if (bitD->ptr - nbBytes < bitD->start) {
      nbBytes = (U32)(bitD->ptr - bitD->start);
      result = BIT_DStream_endOfBuffer;
    }
    bitD->ptr -= nbBytes;
    bitD->bitsConsumed -= nbBytes * 8;
    bitD->bitContainer = MEM_readLEST(bitD->ptr);

    return result;
  }
}

MEM_STATIC unsigned BIT_endOfDStream(const BIT_DStream_t *DStream) {
  return ((DStream->ptr == DStream->start) &&
          (DStream->bitsConsumed == sizeof(DStream->bitContainer) * 8));
}

#endif
