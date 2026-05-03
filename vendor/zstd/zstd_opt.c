/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstd_opt.h"
#include "hist.h"
#include "zstd_compress_internal.h"
#include <string.h>

#if !defined(ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR) ||                         \
    !defined(ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR) ||                           \
    !defined(ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR)

#define ZSTD_LITFREQ_ADD 2

#define ZSTD_MAX_PRICE (1 << 30)

#define ZSTD_PREDEF_THRESHOLD 8

#if 0
#define BITCOST_ACCURACY 0
#define BITCOST_MULTIPLIER (1 << BITCOST_ACCURACY)
#define WEIGHT(stat, opt) ((void)(opt), ZSTD_bitWeight(stat))
#elif 0
#define BITCOST_ACCURACY 8
#define BITCOST_MULTIPLIER (1 << BITCOST_ACCURACY)
#define WEIGHT(stat, opt) ((void)(opt), ZSTD_fracWeight(stat))
#else
#define BITCOST_ACCURACY 8
#define BITCOST_MULTIPLIER (1 << BITCOST_ACCURACY)
#define WEIGHT(stat, opt) ((opt) ? ZSTD_fracWeight(stat) : ZSTD_bitWeight(stat))
#endif

MEM_STATIC U32 ZSTD_bitWeight(U32 stat) {
  return (ZSTD_highbit32(stat + 1) * BITCOST_MULTIPLIER);
}

MEM_STATIC U32 ZSTD_fracWeight(U32 rawStat) {
  U32 const stat = rawStat + 1;
  U32 const hb = ZSTD_highbit32(stat);
  U32 const BWeight = hb * BITCOST_MULTIPLIER;

  U32 const FWeight = (stat << BITCOST_ACCURACY) >> hb;
  U32 const weight = BWeight + FWeight;
  assert(hb + BITCOST_ACCURACY < 31);
  return weight;
}

#if (DEBUGLEVEL >= 2)

MEM_STATIC double ZSTD_fCost(int price) {
  return (double)price / (BITCOST_MULTIPLIER * 8);
}
#endif

static int ZSTD_compressedLiterals(optState_t const *const optPtr) {
  return optPtr->literalCompressionMode != ZSTD_ps_disable;
}

static void ZSTD_setBasePrices(optState_t *optPtr, int optLevel) {
  if (ZSTD_compressedLiterals(optPtr))
    optPtr->litSumBasePrice = WEIGHT(optPtr->litSum, optLevel);
  optPtr->litLengthSumBasePrice = WEIGHT(optPtr->litLengthSum, optLevel);
  optPtr->matchLengthSumBasePrice = WEIGHT(optPtr->matchLengthSum, optLevel);
  optPtr->offCodeSumBasePrice = WEIGHT(optPtr->offCodeSum, optLevel);
}

static U32 sum_u32(const unsigned table[], size_t nbElts) {
  size_t n;
  U32 total = 0;
  for (n = 0; n < nbElts; n++) {
    total += table[n];
  }
  return total;
}

typedef enum { base_0possible = 0, base_1guaranteed = 1 } base_directive_e;

static U32 ZSTD_downscaleStats(unsigned *table, U32 lastEltIndex, U32 shift,
                               base_directive_e base1) {
  U32 s, sum = 0;
  DEBUGLOG(5, "ZSTD_downscaleStats (nbElts=%u, shift=%u)",
           (unsigned)lastEltIndex + 1, (unsigned)shift);
  assert(shift < 30);
  for (s = 0; s < lastEltIndex + 1; s++) {
    unsigned const base = base1 ? 1 : (table[s] > 0);
    unsigned const newStat = base + (table[s] >> shift);
    sum += newStat;
    table[s] = newStat;
  }
  return sum;
}

static U32 ZSTD_scaleStats(unsigned *table, U32 lastEltIndex, U32 logTarget) {
  U32 const prevsum = sum_u32(table, lastEltIndex + 1);
  U32 const factor = prevsum >> logTarget;
  DEBUGLOG(5, "ZSTD_scaleStats (nbElts=%u, target=%u)",
           (unsigned)lastEltIndex + 1, (unsigned)logTarget);
  assert(logTarget < 30);
  if (factor <= 1)
    return prevsum;
  return ZSTD_downscaleStats(table, lastEltIndex, ZSTD_highbit32(factor),
                             base_1guaranteed);
}

static void ZSTD_rescaleFreqs(optState_t *const optPtr, const BYTE *const src,
                              size_t const srcSize, int const optLevel) {
  int const compressedLiterals = ZSTD_compressedLiterals(optPtr);
  DEBUGLOG(5, "ZSTD_rescaleFreqs (srcSize=%u)", (unsigned)srcSize);
  optPtr->priceType = zop_dynamic;

  if (optPtr->litLengthSum == 0) {

    if (srcSize <= ZSTD_PREDEF_THRESHOLD) {
      DEBUGLOG(5, "srcSize <= %i : use predefined stats",
               ZSTD_PREDEF_THRESHOLD);
      optPtr->priceType = zop_predef;
    }

    assert(optPtr->symbolCosts != NULL);
    if (optPtr->symbolCosts->huf.repeatMode == HUF_repeat_valid) {

      optPtr->priceType = zop_dynamic;

      if (compressedLiterals) {

        unsigned lit;
        assert(optPtr->litFreq != NULL);
        optPtr->litSum = 0;
        for (lit = 0; lit <= MaxLit; lit++) {
          U32 const scaleLog = 11;
          U32 const bitCost =
              HUF_getNbBitsFromCTable(optPtr->symbolCosts->huf.CTable, lit);
          assert(bitCost <= scaleLog);
          optPtr->litFreq[lit] = bitCost ? 1 << (scaleLog - bitCost) : 1;
          optPtr->litSum += optPtr->litFreq[lit];
        }
      }

      {
        unsigned ll;
        FSE_CState_t llstate;
        FSE_initCState(&llstate, optPtr->symbolCosts->fse.litlengthCTable);
        optPtr->litLengthSum = 0;
        for (ll = 0; ll <= MaxLL; ll++) {
          U32 const scaleLog = 10;
          U32 const bitCost = FSE_getMaxNbBits(llstate.symbolTT, ll);
          assert(bitCost < scaleLog);
          optPtr->litLengthFreq[ll] = bitCost ? 1 << (scaleLog - bitCost) : 1;
          optPtr->litLengthSum += optPtr->litLengthFreq[ll];
        }
      }

      {
        unsigned ml;
        FSE_CState_t mlstate;
        FSE_initCState(&mlstate, optPtr->symbolCosts->fse.matchlengthCTable);
        optPtr->matchLengthSum = 0;
        for (ml = 0; ml <= MaxML; ml++) {
          U32 const scaleLog = 10;
          U32 const bitCost = FSE_getMaxNbBits(mlstate.symbolTT, ml);
          assert(bitCost < scaleLog);
          optPtr->matchLengthFreq[ml] = bitCost ? 1 << (scaleLog - bitCost) : 1;
          optPtr->matchLengthSum += optPtr->matchLengthFreq[ml];
        }
      }

      {
        unsigned of;
        FSE_CState_t ofstate;
        FSE_initCState(&ofstate, optPtr->symbolCosts->fse.offcodeCTable);
        optPtr->offCodeSum = 0;
        for (of = 0; of <= MaxOff; of++) {
          U32 const scaleLog = 10;
          U32 const bitCost = FSE_getMaxNbBits(ofstate.symbolTT, of);
          assert(bitCost < scaleLog);
          optPtr->offCodeFreq[of] = bitCost ? 1 << (scaleLog - bitCost) : 1;
          optPtr->offCodeSum += optPtr->offCodeFreq[of];
        }
      }

    } else {

      assert(optPtr->litFreq != NULL);
      if (compressedLiterals) {

        unsigned lit = MaxLit;
        HIST_count_simple(optPtr->litFreq, &lit, src, srcSize);
        optPtr->litSum =
            ZSTD_downscaleStats(optPtr->litFreq, MaxLit, 8, base_0possible);
      }

      {
        unsigned const baseLLfreqs[MaxLL + 1] = {
            4, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        ZSTD_memcpy(optPtr->litLengthFreq, baseLLfreqs, sizeof(baseLLfreqs));
        optPtr->litLengthSum = sum_u32(baseLLfreqs, MaxLL + 1);
      }

      {
        unsigned ml;
        for (ml = 0; ml <= MaxML; ml++)
          optPtr->matchLengthFreq[ml] = 1;
      }
      optPtr->matchLengthSum = MaxML + 1;

      {
        unsigned const baseOFCfreqs[MaxOff + 1] = {
            6, 2, 1, 1, 2, 3, 4, 4, 4, 3, 2, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        ZSTD_memcpy(optPtr->offCodeFreq, baseOFCfreqs, sizeof(baseOFCfreqs));
        optPtr->offCodeSum = sum_u32(baseOFCfreqs, MaxOff + 1);
      }
    }

  } else {

    if (compressedLiterals)
      optPtr->litSum = ZSTD_scaleStats(optPtr->litFreq, MaxLit, 12);
    optPtr->litLengthSum = ZSTD_scaleStats(optPtr->litLengthFreq, MaxLL, 11);
    optPtr->matchLengthSum =
        ZSTD_scaleStats(optPtr->matchLengthFreq, MaxML, 11);
    optPtr->offCodeSum = ZSTD_scaleStats(optPtr->offCodeFreq, MaxOff, 11);
  }

  ZSTD_setBasePrices(optPtr, optLevel);
}

static U32 ZSTD_rawLiteralsCost(const BYTE *const literals, U32 const litLength,
                                const optState_t *const optPtr, int optLevel) {
  DEBUGLOG(8, "ZSTD_rawLiteralsCost (%u literals)", litLength);
  if (litLength == 0)
    return 0;

  if (!ZSTD_compressedLiterals(optPtr))
    return (litLength << 3) * BITCOST_MULTIPLIER;

  if (optPtr->priceType == zop_predef)
    return (litLength * 6) * BITCOST_MULTIPLIER;

  {
    U32 price = optPtr->litSumBasePrice * litLength;
    U32 const litPriceMax = optPtr->litSumBasePrice - BITCOST_MULTIPLIER;
    U32 u;
    assert(optPtr->litSumBasePrice >= BITCOST_MULTIPLIER);
    for (u = 0; u < litLength; u++) {
      U32 litPrice = WEIGHT(optPtr->litFreq[literals[u]], optLevel);
      if (UNLIKELY(litPrice > litPriceMax))
        litPrice = litPriceMax;
      price -= litPrice;
    }
    return price;
  }
}

static U32 ZSTD_litLengthPrice(U32 const litLength,
                               const optState_t *const optPtr, int optLevel) {
  assert(litLength <= ZSTD_BLOCKSIZE_MAX);
  if (optPtr->priceType == zop_predef)
    return WEIGHT(litLength, optLevel);

  if (litLength == ZSTD_BLOCKSIZE_MAX)
    return BITCOST_MULTIPLIER +
           ZSTD_litLengthPrice(ZSTD_BLOCKSIZE_MAX - 1, optPtr, optLevel);

  {
    U32 const llCode = ZSTD_LLcode(litLength);
    return (LL_bits[llCode] * BITCOST_MULTIPLIER) +
           optPtr->litLengthSumBasePrice -
           WEIGHT(optPtr->litLengthFreq[llCode], optLevel);
  }
}

FORCE_INLINE_TEMPLATE U32 ZSTD_getMatchPrice(U32 const offBase,
                                             U32 const matchLength,
                                             const optState_t *const optPtr,
                                             int const optLevel) {
  U32 price;
  U32 const offCode = ZSTD_highbit32(offBase);
  U32 const mlBase = matchLength - MINMATCH;
  assert(matchLength >= MINMATCH);

  if (optPtr->priceType == zop_predef)
    return WEIGHT(mlBase, optLevel) + ((16 + offCode) * BITCOST_MULTIPLIER);

  price = (offCode * BITCOST_MULTIPLIER) +
          (optPtr->offCodeSumBasePrice -
           WEIGHT(optPtr->offCodeFreq[offCode], optLevel));
  if ((optLevel < 2) && offCode >= 20)
    price += (offCode - 19) * 2 * BITCOST_MULTIPLIER;

  {
    U32 const mlCode = ZSTD_MLcode(mlBase);
    price += (ML_bits[mlCode] * BITCOST_MULTIPLIER) +
             (optPtr->matchLengthSumBasePrice -
              WEIGHT(optPtr->matchLengthFreq[mlCode], optLevel));
  }

  price += BITCOST_MULTIPLIER / 5;

  DEBUGLOG(8, "ZSTD_getMatchPrice(ml:%u) = %u", matchLength, price);
  return price;
}

static void ZSTD_updateStats(optState_t *const optPtr, U32 litLength,
                             const BYTE *literals, U32 offBase,
                             U32 matchLength) {

  if (ZSTD_compressedLiterals(optPtr)) {
    U32 u;
    for (u = 0; u < litLength; u++)
      optPtr->litFreq[literals[u]] += ZSTD_LITFREQ_ADD;
    optPtr->litSum += litLength * ZSTD_LITFREQ_ADD;
  }

  {
    U32 const llCode = ZSTD_LLcode(litLength);
    optPtr->litLengthFreq[llCode]++;
    optPtr->litLengthSum++;
  }

  {
    U32 const offCode = ZSTD_highbit32(offBase);
    assert(offCode <= MaxOff);
    optPtr->offCodeFreq[offCode]++;
    optPtr->offCodeSum++;
  }

  {
    U32 const mlBase = matchLength - MINMATCH;
    U32 const mlCode = ZSTD_MLcode(mlBase);
    optPtr->matchLengthFreq[mlCode]++;
    optPtr->matchLengthSum++;
  }
}

MEM_STATIC U32 ZSTD_readMINMATCH(const void *memPtr, U32 length) {
  switch (length) {
  default:
  case 4:
    return MEM_read32(memPtr);
  case 3:
    if (MEM_isLittleEndian())
      return MEM_read32(memPtr) << 8;
    else
      return MEM_read32(memPtr) >> 8;
  }
}

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR U32 ZSTD_insertAndFindFirstIndexHash3(
    const ZSTD_MatchState_t *ms, U32 *nextToUpdate3, const BYTE *const ip) {
  U32 *const hashTable3 = ms->hashTable3;
  U32 const hashLog3 = ms->hashLog3;
  const BYTE *const base = ms->window.base;
  U32 idx = *nextToUpdate3;
  U32 const target = (U32)(ip - base);
  size_t const hash3 = ZSTD_hash3Ptr(ip, hashLog3);
  assert(hashLog3 > 0);

  while (idx < target) {
    hashTable3[ZSTD_hash3Ptr(base + idx, hashLog3)] = idx;
    idx++;
  }

  *nextToUpdate3 = target;
  return hashTable3[hash3];
}

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR U32 ZSTD_insertBt1(
    const ZSTD_MatchState_t *ms, const BYTE *const ip, const BYTE *const iend,
    U32 const target, U32 const mls, const int extDict) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hashLog = cParams->hashLog;
  size_t const h = ZSTD_hashPtr(ip, hashLog, mls);
  U32 *const bt = ms->chainTable;
  U32 const btLog = cParams->chainLog - 1;
  U32 const btMask = (1 << btLog) - 1;
  U32 matchIndex = hashTable[h];
  size_t commonLengthSmaller = 0, commonLengthLarger = 0;
  const BYTE *const base = ms->window.base;
  const BYTE *const dictBase = ms->window.dictBase;
  const U32 dictLimit = ms->window.dictLimit;
  const BYTE *const dictEnd = dictBase + dictLimit;
  const BYTE *const prefixStart = base + dictLimit;
  const BYTE *match;
  const U32 curr = (U32)(ip - base);
  const U32 btLow = btMask >= curr ? 0 : curr - btMask;
  U32 *smallerPtr = bt + 2 * (curr & btMask);
  U32 *largerPtr = smallerPtr + 1;
  U32 dummy32;

  U32 const windowLow =
      ZSTD_getLowestMatchIndex(ms, target, cParams->windowLog);
  U32 matchEndIdx = curr + 8 + 1;
  size_t bestLength = 8;
  U32 nbCompares = 1U << cParams->searchLog;
#ifdef ZSTD_C_PREDICT
  U32 predictedSmall = *(bt + 2 * ((curr - 1) & btMask) + 0);
  U32 predictedLarge = *(bt + 2 * ((curr - 1) & btMask) + 1);
  predictedSmall += (predictedSmall > 0);
  predictedLarge += (predictedLarge > 0);
#endif

  DEBUGLOG(8, "ZSTD_insertBt1 (%u)", curr);

  assert(curr <= target);
  assert(ip <= iend - 8);
  hashTable[h] = curr;

  assert(windowLow > 0);
  for (; nbCompares && (matchIndex >= windowLow); --nbCompares) {
    U32 *const nextPtr = bt + 2 * (matchIndex & btMask);
    size_t matchLength = MIN(commonLengthSmaller, commonLengthLarger);
    assert(matchIndex < curr);

#ifdef ZSTD_C_PREDICT
    const U32 *predictPtr = bt + 2 * ((matchIndex - 1) & btMask);
    if (matchIndex == predictedSmall) {

      *smallerPtr = matchIndex;
      if (matchIndex <= btLow) {
        smallerPtr = &dummy32;
        break;
      }
      smallerPtr = nextPtr + 1;
      matchIndex = nextPtr[1];

      predictedSmall = predictPtr[1] + (predictPtr[1] > 0);
      continue;
    }
    if (matchIndex == predictedLarge) {
      *largerPtr = matchIndex;
      if (matchIndex <= btLow) {
        largerPtr = &dummy32;
        break;
      }
      largerPtr = nextPtr;
      matchIndex = nextPtr[0];
      predictedLarge = predictPtr[0] + (predictPtr[0] > 0);
      continue;
    }
#endif

    if (!extDict || (matchIndex + matchLength >= dictLimit)) {
      assert(matchIndex + matchLength >= dictLimit);
      match = base + matchIndex;
      matchLength += ZSTD_count(ip + matchLength, match + matchLength, iend);
    } else {
      match = dictBase + matchIndex;
      matchLength += ZSTD_count_2segments(ip + matchLength, match + matchLength,
                                          iend, dictEnd, prefixStart);
      if (matchIndex + matchLength >= dictLimit)
        match = base + matchIndex;
    }

    if (matchLength > bestLength) {
      bestLength = matchLength;
      if (matchLength > matchEndIdx - matchIndex)
        matchEndIdx = matchIndex + (U32)matchLength;
    }

    if (ip + matchLength == iend) {
      break;
    }

    if (match[matchLength] < ip[matchLength]) {

      *smallerPtr = matchIndex;
      commonLengthSmaller = matchLength;

      if (matchIndex <= btLow) {
        smallerPtr = &dummy32;
        break;
      }
      smallerPtr = nextPtr + 1;

      matchIndex = nextPtr[1];

    } else {

      *largerPtr = matchIndex;
      commonLengthLarger = matchLength;
      if (matchIndex <= btLow) {
        largerPtr = &dummy32;
        break;
      }
      largerPtr = nextPtr;
      matchIndex = nextPtr[0];
    }
  }

  *smallerPtr = *largerPtr = 0;
  {
    U32 positions = 0;
    if (bestLength > 384)
      positions = MIN(192, (U32)(bestLength - 384));
    assert(matchEndIdx > curr + 8);
    return MAX(positions, matchEndIdx - (curr + 8));
  }
}

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
void ZSTD_updateTree_internal(ZSTD_MatchState_t *ms, const BYTE *const ip,
                              const BYTE *const iend, const U32 mls,
                              const ZSTD_dictMode_e dictMode) {
  const BYTE *const base = ms->window.base;
  U32 const target = (U32)(ip - base);
  U32 idx = ms->nextToUpdate;
  DEBUGLOG(7, "ZSTD_updateTree_internal, from %u to %u  (dictMode:%u)", idx,
           target, dictMode);

  while (idx < target) {
    U32 const forward = ZSTD_insertBt1(ms, base + idx, iend, target, mls,
                                       dictMode == ZSTD_extDict);
    assert(idx < (U32)(idx + forward));
    idx += forward;
  }
  assert((size_t)(ip - base) <= (size_t)(U32)(-1));
  assert((size_t)(iend - base) <= (size_t)(U32)(-1));
  ms->nextToUpdate = target;
}

void ZSTD_updateTree(ZSTD_MatchState_t *ms, const BYTE *ip, const BYTE *iend) {
  ZSTD_updateTree_internal(ms, ip, iend, ms->cParams.minMatch, ZSTD_noDict);
}

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
U32 ZSTD_insertBtAndGetAllMatches(ZSTD_match_t *matches,

                                  ZSTD_MatchState_t *ms, U32 *nextToUpdate3,
                                  const BYTE *const ip,
                                  const BYTE *const iLimit,
                                  const ZSTD_dictMode_e dictMode,
                                  const U32 rep[ZSTD_REP_NUM], const U32 ll0,

                                  const U32 lengthToBeat, const U32 mls) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 const sufficient_len = MIN(cParams->targetLength, ZSTD_OPT_NUM - 1);
  const BYTE *const base = ms->window.base;
  U32 const curr = (U32)(ip - base);
  U32 const hashLog = cParams->hashLog;
  U32 const minMatch = (mls == 3) ? 3 : 4;
  U32 *const hashTable = ms->hashTable;
  size_t const h = ZSTD_hashPtr(ip, hashLog, mls);
  U32 matchIndex = hashTable[h];
  U32 *const bt = ms->chainTable;
  U32 const btLog = cParams->chainLog - 1;
  U32 const btMask = (1U << btLog) - 1;
  size_t commonLengthSmaller = 0, commonLengthLarger = 0;
  const BYTE *const dictBase = ms->window.dictBase;
  U32 const dictLimit = ms->window.dictLimit;
  const BYTE *const dictEnd = dictBase + dictLimit;
  const BYTE *const prefixStart = base + dictLimit;
  U32 const btLow = (btMask >= curr) ? 0 : curr - btMask;
  U32 const windowLow = ZSTD_getLowestMatchIndex(ms, curr, cParams->windowLog);
  U32 const matchLow = windowLow ? windowLow : 1;
  U32 *smallerPtr = bt + 2 * (curr & btMask);
  U32 *largerPtr = bt + 2 * (curr & btMask) + 1;
  U32 matchEndIdx = curr + 8 + 1;

  U32 dummy32;
  U32 mnum = 0;
  U32 nbCompares = 1U << cParams->searchLog;

  const ZSTD_MatchState_t *dms =
      dictMode == ZSTD_dictMatchState ? ms->dictMatchState : NULL;
  const BYTE *const dmsBase =
      dictMode == ZSTD_dictMatchState ? ms->dictMatchState->window.base : NULL;
  const BYTE *const dmsEnd = dictMode == ZSTD_dictMatchState
                                 ? ms->dictMatchState->window.nextSrc
                                 : NULL;
  U32 const dmsHighLimit = dictMode == ZSTD_dictMatchState
                               ? (U32)(ms->dictMatchState->window.nextSrc -
                                       ms->dictMatchState->window.base)
                               : 0;
  U32 const dmsLowLimit =
      dictMode == ZSTD_dictMatchState ? ms->dictMatchState->window.lowLimit : 0;
  U32 const dmsIndexDelta =
      dictMode == ZSTD_dictMatchState ? windowLow - dmsHighLimit : 0;
  U32 const dmsHashLog = dictMode == ZSTD_dictMatchState
                             ? ms->dictMatchState->cParams.hashLog
                             : hashLog;
  U32 const dmsBtLog = dictMode == ZSTD_dictMatchState
                           ? ms->dictMatchState->cParams.chainLog - 1
                           : btLog;
  U32 const dmsBtMask =
      dictMode == ZSTD_dictMatchState ? (1U << dmsBtLog) - 1 : 0;
  U32 const dmsBtLow =
      dictMode == ZSTD_dictMatchState && dmsBtMask < dmsHighLimit - dmsLowLimit
          ? dmsHighLimit - dmsBtMask
          : dmsLowLimit;

  size_t bestLength = lengthToBeat - 1;
  DEBUGLOG(8, "ZSTD_insertBtAndGetAllMatches: current=%u", curr);

  assert(ll0 <= 1);
  {
    U32 const lastR = ZSTD_REP_NUM + ll0;
    U32 repCode;
    for (repCode = ll0; repCode < lastR; repCode++) {
      U32 const repOffset =
          (repCode == ZSTD_REP_NUM) ? (rep[0] - 1) : rep[repCode];
      U32 const repIndex = curr - repOffset;
      U32 repLen = 0;
      assert(curr >= dictLimit);
      if (repOffset - 1 < curr - dictLimit) {

        if ((repIndex >= windowLow) &
            (ZSTD_readMINMATCH(ip, minMatch) ==
             ZSTD_readMINMATCH(ip - repOffset, minMatch))) {
          repLen = (U32)ZSTD_count(ip + minMatch, ip + minMatch - repOffset,
                                   iLimit) +
                   minMatch;
        }
      } else {
        const BYTE *const repMatch = dictMode == ZSTD_dictMatchState
                                         ? dmsBase + repIndex - dmsIndexDelta
                                         : dictBase + repIndex;
        assert(curr >= windowLow);
        if (dictMode == ZSTD_extDict &&
            (((repOffset - 1) < curr - windowLow) &
             (ZSTD_index_overlap_check(dictLimit, repIndex))) &&
            (ZSTD_readMINMATCH(ip, minMatch) ==
             ZSTD_readMINMATCH(repMatch, minMatch))) {
          repLen = (U32)ZSTD_count_2segments(ip + minMatch, repMatch + minMatch,
                                             iLimit, dictEnd, prefixStart) +
                   minMatch;
        }
        if (dictMode == ZSTD_dictMatchState &&
            (((repOffset - 1) < curr - (dmsLowLimit + dmsIndexDelta))

             & (ZSTD_index_overlap_check(dictLimit, repIndex))) &&
            (ZSTD_readMINMATCH(ip, minMatch) ==
             ZSTD_readMINMATCH(repMatch, minMatch))) {
          repLen = (U32)ZSTD_count_2segments(ip + minMatch, repMatch + minMatch,
                                             iLimit, dmsEnd, prefixStart) +
                   minMatch;
        }
      }

      if (repLen > bestLength) {
        DEBUGLOG(8, "found repCode %u (ll0:%u, offset:%u) of length %u",
                 repCode, ll0, repOffset, repLen);
        bestLength = repLen;
        matches[mnum].off = REPCODE_TO_OFFBASE(repCode - ll0 + 1);
        matches[mnum].len = (U32)repLen;
        mnum++;
        if ((repLen > sufficient_len) | (ip + repLen == iLimit)) {
          return mnum;
        }
      }
    }
  }

  if ((mls == 3) && (bestLength < mls)) {
    U32 const matchIndex3 =
        ZSTD_insertAndFindFirstIndexHash3(ms, nextToUpdate3, ip);
    if ((matchIndex3 >= matchLow) & (curr - matchIndex3 < (1 << 18))) {
      size_t mlen;
      if ((dictMode == ZSTD_noDict) || (dictMode == ZSTD_dictMatchState) ||
          (matchIndex3 >= dictLimit)) {
        const BYTE *const match = base + matchIndex3;
        mlen = ZSTD_count(ip, match, iLimit);
      } else {
        const BYTE *const match = dictBase + matchIndex3;
        mlen = ZSTD_count_2segments(ip, match, iLimit, dictEnd, prefixStart);
      }

      if (mlen >= mls) {
        DEBUGLOG(8, "found small match with hlog3, of length %u", (U32)mlen);
        bestLength = mlen;
        assert(curr > matchIndex3);
        assert(mnum == 0);
        matches[0].off = OFFSET_TO_OFFBASE(curr - matchIndex3);
        matches[0].len = (U32)mlen;
        mnum = 1;
        if ((mlen > sufficient_len) | (ip + mlen == iLimit)) {
          ms->nextToUpdate = curr + 1;
          return 1;
        }
      }
    }
  }

  hashTable[h] = curr;

  for (; nbCompares && (matchIndex >= matchLow); --nbCompares) {
    U32 *const nextPtr = bt + 2 * (matchIndex & btMask);
    const BYTE *match;
    size_t matchLength = MIN(commonLengthSmaller, commonLengthLarger);
    assert(curr > matchIndex);

    if ((dictMode == ZSTD_noDict) || (dictMode == ZSTD_dictMatchState) ||
        (matchIndex + matchLength >= dictLimit)) {
      assert(matchIndex + matchLength >= dictLimit);
      match = base + matchIndex;
      if (matchIndex >= dictLimit)
        assert(memcmp(match, ip, matchLength) == 0);
      matchLength += ZSTD_count(ip + matchLength, match + matchLength, iLimit);
    } else {
      match = dictBase + matchIndex;
      assert(memcmp(match, ip, matchLength) == 0);
      matchLength += ZSTD_count_2segments(ip + matchLength, match + matchLength,
                                          iLimit, dictEnd, prefixStart);
      if (matchIndex + matchLength >= dictLimit)
        match = base + matchIndex;
    }

    if (matchLength > bestLength) {
      DEBUGLOG(8, "found match of length %u at distance %u (offBase=%u)",
               (U32)matchLength, curr - matchIndex,
               OFFSET_TO_OFFBASE(curr - matchIndex));
      assert(matchEndIdx > matchIndex);
      if (matchLength > matchEndIdx - matchIndex)
        matchEndIdx = matchIndex + (U32)matchLength;
      bestLength = matchLength;
      matches[mnum].off = OFFSET_TO_OFFBASE(curr - matchIndex);
      matches[mnum].len = (U32)matchLength;
      mnum++;
      if ((matchLength > ZSTD_OPT_NUM) | (ip + matchLength == iLimit)) {
        if (dictMode == ZSTD_dictMatchState)
          nbCompares = 0;
        break;
      }
    }

    if (match[matchLength] < ip[matchLength]) {

      *smallerPtr = matchIndex;
      commonLengthSmaller = matchLength;

      if (matchIndex <= btLow) {
        smallerPtr = &dummy32;
        break;
      }
      smallerPtr = nextPtr + 1;

      matchIndex = nextPtr[1];

    } else {
      *largerPtr = matchIndex;
      commonLengthLarger = matchLength;
      if (matchIndex <= btLow) {
        largerPtr = &dummy32;
        break;
      }
      largerPtr = nextPtr;
      matchIndex = nextPtr[0];
    }
  }

  *smallerPtr = *largerPtr = 0;

  assert(nbCompares <= (1U << ZSTD_SEARCHLOG_MAX));
  if (dictMode == ZSTD_dictMatchState && nbCompares) {
    size_t const dmsH = ZSTD_hashPtr(ip, dmsHashLog, mls);
    U32 dictMatchIndex = dms->hashTable[dmsH];
    const U32 *const dmsBt = dms->chainTable;
    commonLengthSmaller = commonLengthLarger = 0;
    for (; nbCompares && (dictMatchIndex > dmsLowLimit); --nbCompares) {
      const U32 *const nextPtr = dmsBt + 2 * (dictMatchIndex & dmsBtMask);
      size_t matchLength = MIN(commonLengthSmaller, commonLengthLarger);
      const BYTE *match = dmsBase + dictMatchIndex;
      matchLength += ZSTD_count_2segments(ip + matchLength, match + matchLength,
                                          iLimit, dmsEnd, prefixStart);
      if (dictMatchIndex + matchLength >= dmsHighLimit)
        match = base + dictMatchIndex + dmsIndexDelta;

      if (matchLength > bestLength) {
        matchIndex = dictMatchIndex + dmsIndexDelta;
        DEBUGLOG(8, "found dms match of length %u at distance %u (offBase=%u)",
                 (U32)matchLength, curr - matchIndex,
                 OFFSET_TO_OFFBASE(curr - matchIndex));
        if (matchLength > matchEndIdx - matchIndex)
          matchEndIdx = matchIndex + (U32)matchLength;
        bestLength = matchLength;
        matches[mnum].off = OFFSET_TO_OFFBASE(curr - matchIndex);
        matches[mnum].len = (U32)matchLength;
        mnum++;
        if ((matchLength > ZSTD_OPT_NUM) | (ip + matchLength == iLimit)) {
          break;
        }
      }

      if (dictMatchIndex <= dmsBtLow) {
        break;
      }
      if (match[matchLength] < ip[matchLength]) {
        commonLengthSmaller = matchLength;

        dictMatchIndex = nextPtr[1];

      } else {

        commonLengthLarger = matchLength;
        dictMatchIndex = nextPtr[0];
      }
    }
  }

  assert(matchEndIdx > curr + 8);
  ms->nextToUpdate = matchEndIdx - 8;
  return mnum;
}

typedef U32 (*ZSTD_getAllMatchesFn)(ZSTD_match_t *, ZSTD_MatchState_t *, U32 *,
                                    const BYTE *, const BYTE *,
                                    const U32 rep[ZSTD_REP_NUM], U32 const ll0,
                                    U32 const lengthToBeat);

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
U32 ZSTD_btGetAllMatches_internal(ZSTD_match_t *matches, ZSTD_MatchState_t *ms,
                                  U32 *nextToUpdate3, const BYTE *ip,
                                  const BYTE *const iHighLimit,
                                  const U32 rep[ZSTD_REP_NUM], U32 const ll0,
                                  U32 const lengthToBeat,
                                  const ZSTD_dictMode_e dictMode,
                                  const U32 mls) {
  assert(BOUNDED(3, ms->cParams.minMatch, 6) == mls);
  DEBUGLOG(8, "ZSTD_BtGetAllMatches(dictMode=%d, mls=%u)", (int)dictMode, mls);
  if (ip < ms->window.base + ms->nextToUpdate)
    return 0;
  ZSTD_updateTree_internal(ms, ip, iHighLimit, mls, dictMode);
  return ZSTD_insertBtAndGetAllMatches(matches, ms, nextToUpdate3, ip,
                                       iHighLimit, dictMode, rep, ll0,
                                       lengthToBeat, mls);
}

#define ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, mls)                              \
  ZSTD_btGetAllMatches_##dictMode##_##mls

#define GEN_ZSTD_BT_GET_ALL_MATCHES_(dictMode, mls)                            \
  static U32 ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, mls)(                        \
      ZSTD_match_t * matches, ZSTD_MatchState_t * ms, U32 * nextToUpdate3,     \
      const BYTE *ip, const BYTE *const iHighLimit,                            \
      const U32 rep[ZSTD_REP_NUM], U32 const ll0, U32 const lengthToBeat) {    \
    return ZSTD_btGetAllMatches_internal(matches, ms, nextToUpdate3, ip,       \
                                         iHighLimit, rep, ll0, lengthToBeat,   \
                                         ZSTD_##dictMode, mls);                \
  }

#define GEN_ZSTD_BT_GET_ALL_MATCHES(dictMode)                                  \
  GEN_ZSTD_BT_GET_ALL_MATCHES_(dictMode, 3)                                    \
  GEN_ZSTD_BT_GET_ALL_MATCHES_(dictMode, 4)                                    \
  GEN_ZSTD_BT_GET_ALL_MATCHES_(dictMode, 5)                                    \
  GEN_ZSTD_BT_GET_ALL_MATCHES_(dictMode, 6)

GEN_ZSTD_BT_GET_ALL_MATCHES(noDict)
GEN_ZSTD_BT_GET_ALL_MATCHES(extDict)
GEN_ZSTD_BT_GET_ALL_MATCHES(dictMatchState)

#define ZSTD_BT_GET_ALL_MATCHES_ARRAY(dictMode)                                \
  {ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, 3),                                    \
   ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, 4),                                    \
   ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, 5),                                    \
   ZSTD_BT_GET_ALL_MATCHES_FN(dictMode, 6)}

static ZSTD_getAllMatchesFn
ZSTD_selectBtGetAllMatches(ZSTD_MatchState_t const *ms,
                           ZSTD_dictMode_e const dictMode) {
  ZSTD_getAllMatchesFn const getAllMatchesFns[3][4] = {
      ZSTD_BT_GET_ALL_MATCHES_ARRAY(noDict),
      ZSTD_BT_GET_ALL_MATCHES_ARRAY(extDict),
      ZSTD_BT_GET_ALL_MATCHES_ARRAY(dictMatchState)};
  U32 const mls = BOUNDED(3, ms->cParams.minMatch, 6);
  assert((U32)dictMode < 3);
  assert(mls - 3 < 4);
  return getAllMatchesFns[(int)dictMode][mls - 3];
}

typedef struct {
  RawSeqStore_t seqStore;
  U32 startPosInBlock;
  U32 endPosInBlock;
  U32 offset;
} ZSTD_optLdm_t;

static void ZSTD_optLdm_skipRawSeqStoreBytes(RawSeqStore_t *rawSeqStore,
                                             size_t nbBytes) {
  U32 currPos = (U32)(rawSeqStore->posInSequence + nbBytes);
  while (currPos && rawSeqStore->pos < rawSeqStore->size) {
    rawSeq currSeq = rawSeqStore->seq[rawSeqStore->pos];
    if (currPos >= currSeq.litLength + currSeq.matchLength) {
      currPos -= currSeq.litLength + currSeq.matchLength;
      rawSeqStore->pos++;
    } else {
      rawSeqStore->posInSequence = currPos;
      break;
    }
  }
  if (currPos == 0 || rawSeqStore->pos == rawSeqStore->size) {
    rawSeqStore->posInSequence = 0;
  }
}

static void ZSTD_opt_getNextMatchAndUpdateSeqStore(ZSTD_optLdm_t *optLdm,
                                                   U32 currPosInBlock,
                                                   U32 blockBytesRemaining) {
  rawSeq currSeq;
  U32 currBlockEndPos;
  U32 literalsBytesRemaining;
  U32 matchBytesRemaining;

  if (optLdm->seqStore.size == 0 ||
      optLdm->seqStore.pos >= optLdm->seqStore.size) {
    optLdm->startPosInBlock = UINT_MAX;
    optLdm->endPosInBlock = UINT_MAX;
    return;
  }

  currSeq = optLdm->seqStore.seq[optLdm->seqStore.pos];
  assert(optLdm->seqStore.posInSequence <=
         currSeq.litLength + currSeq.matchLength);
  currBlockEndPos = currPosInBlock + blockBytesRemaining;
  literalsBytesRemaining =
      (optLdm->seqStore.posInSequence < currSeq.litLength)
          ? currSeq.litLength - (U32)optLdm->seqStore.posInSequence
          : 0;
  matchBytesRemaining =
      (literalsBytesRemaining == 0)
          ? currSeq.matchLength -
                ((U32)optLdm->seqStore.posInSequence - currSeq.litLength)
          : currSeq.matchLength;

  if (literalsBytesRemaining >= blockBytesRemaining) {
    optLdm->startPosInBlock = UINT_MAX;
    optLdm->endPosInBlock = UINT_MAX;
    ZSTD_optLdm_skipRawSeqStoreBytes(&optLdm->seqStore, blockBytesRemaining);
    return;
  }

  optLdm->startPosInBlock = currPosInBlock + literalsBytesRemaining;
  optLdm->endPosInBlock = optLdm->startPosInBlock + matchBytesRemaining;
  optLdm->offset = currSeq.offset;

  if (optLdm->endPosInBlock > currBlockEndPos) {

    optLdm->endPosInBlock = currBlockEndPos;
    ZSTD_optLdm_skipRawSeqStoreBytes(&optLdm->seqStore,
                                     currBlockEndPos - currPosInBlock);
  } else {

    ZSTD_optLdm_skipRawSeqStoreBytes(
        &optLdm->seqStore, literalsBytesRemaining + matchBytesRemaining);
  }
}

static void ZSTD_optLdm_maybeAddMatch(ZSTD_match_t *matches, U32 *nbMatches,
                                      const ZSTD_optLdm_t *optLdm,
                                      U32 currPosInBlock, U32 minMatch) {
  U32 const posDiff = currPosInBlock - optLdm->startPosInBlock;

  U32 const candidateMatchLength =
      optLdm->endPosInBlock - optLdm->startPosInBlock - posDiff;

  if (currPosInBlock < optLdm->startPosInBlock ||
      currPosInBlock >= optLdm->endPosInBlock ||
      candidateMatchLength < minMatch) {
    return;
  }

  if (*nbMatches == 0 ||
      ((candidateMatchLength > matches[*nbMatches - 1].len) &&
       *nbMatches < ZSTD_OPT_NUM)) {
    U32 const candidateOffBase = OFFSET_TO_OFFBASE(optLdm->offset);
    DEBUGLOG(6,
             "ZSTD_optLdm_maybeAddMatch(): Adding ldm candidate match "
             "(offBase: %u matchLength %u) at block position=%u",
             candidateOffBase, candidateMatchLength, currPosInBlock);
    matches[*nbMatches].len = candidateMatchLength;
    matches[*nbMatches].off = candidateOffBase;
    (*nbMatches)++;
  }
}

static void
ZSTD_optLdm_processMatchCandidate(ZSTD_optLdm_t *optLdm, ZSTD_match_t *matches,
                                  U32 *nbMatches, U32 currPosInBlock,
                                  U32 remainingBytes, U32 minMatch) {
  if (optLdm->seqStore.size == 0 ||
      optLdm->seqStore.pos >= optLdm->seqStore.size) {
    return;
  }

  if (currPosInBlock >= optLdm->endPosInBlock) {
    if (currPosInBlock > optLdm->endPosInBlock) {

      U32 const posOvershoot = currPosInBlock - optLdm->endPosInBlock;
      ZSTD_optLdm_skipRawSeqStoreBytes(&optLdm->seqStore, posOvershoot);
    }
    ZSTD_opt_getNextMatchAndUpdateSeqStore(optLdm, currPosInBlock,
                                           remainingBytes);
  }
  ZSTD_optLdm_maybeAddMatch(matches, nbMatches, optLdm, currPosInBlock,
                            minMatch);
}

#if 0 

static void
listStats(const U32* table, int lastEltID)
{
    int const nbElts = lastEltID + 1;
    int enb;
    for (enb=0; enb < nbElts; enb++) {
        (void)table;
        
        RAWLOG(2, "%4i,", table[enb]);
    }
    RAWLOG(2, " \n");
}

#endif

#define LIT_PRICE(_p) (int)ZSTD_rawLiteralsCost(_p, 1, optStatePtr, optLevel)
#define LL_PRICE(_l) (int)ZSTD_litLengthPrice(_l, optStatePtr, optLevel)
#define LL_INCPRICE(_l) (LL_PRICE(_l) - LL_PRICE(_l - 1))

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
size_t ZSTD_compressBlock_opt_generic(ZSTD_MatchState_t *ms,
                                      SeqStore_t *seqStore,
                                      U32 rep[ZSTD_REP_NUM], const void *src,
                                      size_t srcSize, const int optLevel,
                                      const ZSTD_dictMode_e dictMode) {
  optState_t *const optStatePtr = &ms->opt;
  const BYTE *const istart = (const BYTE *)src;
  const BYTE *ip = istart;
  const BYTE *anchor = istart;
  const BYTE *const iend = istart + srcSize;
  const BYTE *const ilimit = iend - 8;
  const BYTE *const base = ms->window.base;
  const BYTE *const prefixStart = base + ms->window.dictLimit;
  const ZSTD_compressionParameters *const cParams = &ms->cParams;

  ZSTD_getAllMatchesFn getAllMatches = ZSTD_selectBtGetAllMatches(ms, dictMode);

  U32 const sufficient_len = MIN(cParams->targetLength, ZSTD_OPT_NUM - 1);
  U32 const minMatch = (cParams->minMatch == 3) ? 3 : 4;
  U32 nextToUpdate3 = ms->nextToUpdate;

  ZSTD_optimal_t *const opt = optStatePtr->priceTable;
  ZSTD_match_t *const matches = optStatePtr->matchTable;
  ZSTD_optimal_t lastStretch;
  ZSTD_optLdm_t optLdm;

  ZSTD_memset(&lastStretch, 0, sizeof(ZSTD_optimal_t));

  optLdm.seqStore = ms->ldmSeqStore ? *ms->ldmSeqStore : kNullRawSeqStore;
  optLdm.endPosInBlock = optLdm.startPosInBlock = optLdm.offset = 0;
  ZSTD_opt_getNextMatchAndUpdateSeqStore(&optLdm, (U32)(ip - istart),
                                         (U32)(iend - ip));

  DEBUGLOG(
      5,
      "ZSTD_compressBlock_opt_generic: current=%u, prefix=%u, nextToUpdate=%u",
      (U32)(ip - base), ms->window.dictLimit, ms->nextToUpdate);
  assert(optLevel <= 2);
  ZSTD_rescaleFreqs(optStatePtr, (const BYTE *)src, srcSize, optLevel);
  ip += (ip == prefixStart);

  while (ip < ilimit) {
    U32 cur, last_pos = 0;

    {
      U32 const litlen = (U32)(ip - anchor);
      U32 const ll0 = !litlen;
      U32 nbMatches = getAllMatches(matches, ms, &nextToUpdate3, ip, iend, rep,
                                    ll0, minMatch);
      ZSTD_optLdm_processMatchCandidate(&optLdm, matches, &nbMatches,
                                        (U32)(ip - istart), (U32)(iend - ip),
                                        minMatch);
      if (!nbMatches) {
        DEBUGLOG(8, "no match found at cPos %u", (unsigned)(ip - istart));
        ip++;
        continue;
      }

      opt[0].mlen = 0;
      opt[0].litlen = litlen;

      opt[0].price = LL_PRICE(litlen);
      ZSTD_STATIC_ASSERT(sizeof(opt[0].rep[0]) == sizeof(rep[0]));
      ZSTD_memcpy(&opt[0].rep, rep, sizeof(opt[0].rep));

      {
        U32 const maxML = matches[nbMatches - 1].len;
        U32 const maxOffBase = matches[nbMatches - 1].off;
        DEBUGLOG(6,
                 "found %u matches of maxLength=%u and maxOffBase=%u at "
                 "cPos=%u => start new series",
                 nbMatches, maxML, maxOffBase, (U32)(ip - prefixStart));

        if (maxML > sufficient_len) {
          lastStretch.litlen = 0;
          lastStretch.mlen = maxML;
          lastStretch.off = maxOffBase;
          DEBUGLOG(6, "large match (%u>%u) => immediate encoding", maxML,
                   sufficient_len);
          cur = 0;
          last_pos = maxML;
          goto _shortestPath;
        }
      }

      assert(opt[0].price >= 0);
      {
        U32 pos;
        U32 matchNb;
        for (pos = 1; pos < minMatch; pos++) {
          opt[pos].price = ZSTD_MAX_PRICE;
          opt[pos].mlen = 0;
          opt[pos].litlen = litlen + pos;
        }
        for (matchNb = 0; matchNb < nbMatches; matchNb++) {
          U32 const offBase = matches[matchNb].off;
          U32 const end = matches[matchNb].len;
          for (; pos <= end; pos++) {
            int const matchPrice =
                (int)ZSTD_getMatchPrice(offBase, pos, optStatePtr, optLevel);
            int const sequencePrice = opt[0].price + matchPrice;
            DEBUGLOG(7, "rPos:%u => set initial price : %.2f", pos,
                     ZSTD_fCost(sequencePrice));
            opt[pos].mlen = pos;
            opt[pos].off = offBase;
            opt[pos].litlen = 0;
            opt[pos].price = sequencePrice + LL_PRICE(0);
          }
        }
        last_pos = pos - 1;
        opt[pos].price = ZSTD_MAX_PRICE;
      }
    }

    for (cur = 1; cur <= last_pos; cur++) {
      const BYTE *const inr = ip + cur;
      assert(cur <= ZSTD_OPT_NUM);
      DEBUGLOG(7, "cPos:%i==rPos:%u", (int)(inr - istart), cur);

      {
        U32 const litlen = opt[cur - 1].litlen + 1;
        int const price =
            opt[cur - 1].price + LIT_PRICE(ip + cur - 1) + LL_INCPRICE(litlen);
        assert(price < 1000000000);
        if (price <= opt[cur].price) {
          ZSTD_optimal_t const prevMatch = opt[cur];
          DEBUGLOG(7,
                   "cPos:%i==rPos:%u : better price (%.2f<=%.2f) using literal "
                   "(ll==%u) (hist:%u,%u,%u)",
                   (int)(inr - istart), cur, ZSTD_fCost(price),
                   ZSTD_fCost(opt[cur].price), litlen, opt[cur - 1].rep[0],
                   opt[cur - 1].rep[1], opt[cur - 1].rep[2]);
          opt[cur] = opt[cur - 1];
          opt[cur].litlen = litlen;
          opt[cur].price = price;
          if ((optLevel >= 1) && (prevMatch.litlen == 0) &&
              (LL_INCPRICE(1) < 0) && LIKELY(ip + cur < iend)) {

            int with1literal =
                prevMatch.price + LIT_PRICE(ip + cur) + LL_INCPRICE(1);
            int withMoreLiterals =
                price + LIT_PRICE(ip + cur) + LL_INCPRICE(litlen + 1);
            DEBUGLOG(7, "then at next rPos %u : match+1lit %.2f vs %ulits %.2f",
                     cur + 1, ZSTD_fCost(with1literal), litlen + 1,
                     ZSTD_fCost(withMoreLiterals));
            if ((with1literal < withMoreLiterals) &&
                (with1literal < opt[cur + 1].price)) {

              U32 const prev = cur - prevMatch.mlen;
              Repcodes_t const newReps = ZSTD_newRep(
                  opt[prev].rep, prevMatch.off, opt[prev].litlen == 0);
              assert(cur >= prevMatch.mlen);
              DEBUGLOG(
                  7,
                  "==> match+1lit is cheaper (%.2f < %.2f) (hist:%u,%u,%u) !",
                  ZSTD_fCost(with1literal), ZSTD_fCost(withMoreLiterals),
                  newReps.rep[0], newReps.rep[1], newReps.rep[2]);
              opt[cur + 1] = prevMatch;
              ZSTD_memcpy(opt[cur + 1].rep, &newReps, sizeof(Repcodes_t));
              opt[cur + 1].litlen = 1;
              opt[cur + 1].price = with1literal;
              if (last_pos < cur + 1)
                last_pos = cur + 1;
            }
          }
        } else {
          DEBUGLOG(7, "cPos:%i==rPos:%u : literal would cost more (%.2f>%.2f)",
                   (int)(inr - istart), cur, ZSTD_fCost(price),
                   ZSTD_fCost(opt[cur].price));
        }
      }

      ZSTD_STATIC_ASSERT(sizeof(opt[cur].rep) == sizeof(Repcodes_t));
      assert(cur >= opt[cur].mlen);
      if (opt[cur].litlen == 0) {

        U32 const prev = cur - opt[cur].mlen;
        Repcodes_t const newReps =
            ZSTD_newRep(opt[prev].rep, opt[cur].off, opt[prev].litlen == 0);
        ZSTD_memcpy(opt[cur].rep, &newReps, sizeof(Repcodes_t));
      }

      if (inr > ilimit)
        continue;

      if (cur == last_pos)
        break;

      if ((optLevel == 0) &&
          (opt[cur + 1].price <= opt[cur].price + (BITCOST_MULTIPLIER / 2))) {
        DEBUGLOG(7, "skip current position : next rPos(%u) price is cheaper",
                 cur + 1);
        continue;
      }

      assert(opt[cur].price >= 0);
      {
        U32 const ll0 = (opt[cur].litlen == 0);
        int const previousPrice = opt[cur].price;
        int const basePrice = previousPrice + LL_PRICE(0);
        U32 nbMatches = getAllMatches(matches, ms, &nextToUpdate3, inr, iend,
                                      opt[cur].rep, ll0, minMatch);
        U32 matchNb;

        ZSTD_optLdm_processMatchCandidate(&optLdm, matches, &nbMatches,
                                          (U32)(inr - istart),
                                          (U32)(iend - inr), minMatch);

        if (!nbMatches) {
          DEBUGLOG(7, "rPos:%u : no match found", cur);
          continue;
        }

        {
          U32 const longestML = matches[nbMatches - 1].len;
          DEBUGLOG(7, "cPos:%i==rPos:%u, found %u matches, of longest ML=%u",
                   (int)(inr - istart), cur, nbMatches, longestML);

          if ((longestML > sufficient_len) ||
              (cur + longestML >= ZSTD_OPT_NUM) ||
              (ip + cur + longestML >= iend)) {
            lastStretch.mlen = longestML;
            lastStretch.off = matches[nbMatches - 1].off;
            lastStretch.litlen = 0;
            last_pos = cur + longestML;
            goto _shortestPath;
          }
        }

        for (matchNb = 0; matchNb < nbMatches; matchNb++) {
          U32 const offset = matches[matchNb].off;
          U32 const lastML = matches[matchNb].len;
          U32 const startML =
              (matchNb > 0) ? matches[matchNb - 1].len + 1 : minMatch;
          U32 mlen;

          DEBUGLOG(7, "testing match %u => offBase=%4u, mlen=%2u, llen=%2u",
                   matchNb, matches[matchNb].off, lastML, opt[cur].litlen);

          for (mlen = lastML; mlen >= startML; mlen--) {
            U32 const pos = cur + mlen;
            int const price =
                basePrice +
                (int)ZSTD_getMatchPrice(offset, mlen, optStatePtr, optLevel);

            if ((pos > last_pos) || (price < opt[pos].price)) {
              DEBUGLOG(7, "rPos:%u (ml=%2u) => new better price (%.2f<%.2f)",
                       pos, mlen, ZSTD_fCost(price),
                       ZSTD_fCost(opt[pos].price));
              while (last_pos < pos) {

                last_pos++;
                opt[last_pos].price = ZSTD_MAX_PRICE;
                opt[last_pos].litlen = !0;
              }
              opt[pos].mlen = mlen;
              opt[pos].off = offset;
              opt[pos].litlen = 0;
              opt[pos].price = price;
            } else {
              DEBUGLOG(7, "rPos:%u (ml=%2u) => new price is worse (%.2f>=%.2f)",
                       pos, mlen, ZSTD_fCost(price),
                       ZSTD_fCost(opt[pos].price));
              if (optLevel == 0)
                break;
            }
          }
        }
      }
      opt[last_pos + 1].price = ZSTD_MAX_PRICE;
    }

    lastStretch = opt[last_pos];
    assert(cur >= lastStretch.mlen);
    cur = last_pos - lastStretch.mlen;

  _shortestPath:
    assert(opt[0].mlen == 0);
    assert(last_pos >= lastStretch.mlen);
    assert(cur == last_pos - lastStretch.mlen);

    if (lastStretch.mlen == 0) {

      assert(lastStretch.litlen == (ip - anchor) + last_pos);
      ip += last_pos;
      continue;
    }
    assert(lastStretch.off > 0);

    if (lastStretch.litlen == 0) {

      Repcodes_t const reps =
          ZSTD_newRep(opt[cur].rep, lastStretch.off, opt[cur].litlen == 0);
      ZSTD_memcpy(rep, &reps, sizeof(Repcodes_t));
    } else {
      ZSTD_memcpy(rep, lastStretch.rep, sizeof(Repcodes_t));
      assert(cur >= lastStretch.litlen);
      cur -= lastStretch.litlen;
    }

    {
      U32 const storeEnd = cur + 2;
      U32 storeStart = storeEnd;
      U32 stretchPos = cur;

      DEBUGLOG(6, "start reverse traversal (last_pos:%u, cur:%u)", last_pos,
               cur);
      (void)last_pos;
      assert(storeEnd < ZSTD_OPT_SIZE);
      DEBUGLOG(6, "last stretch copied into pos=%u (llen=%u,mlen=%u,ofc=%u)",
               storeEnd, lastStretch.litlen, lastStretch.mlen, lastStretch.off);
      opt[storeEnd] = lastStretch;
      storeStart = storeEnd;
      while (1) {
        ZSTD_optimal_t nextStretch = opt[stretchPos];
        opt[storeStart].litlen = nextStretch.litlen;
        DEBUGLOG(6, "selected sequence (llen=%u,mlen=%u,ofc=%u)",
                 opt[storeStart].litlen, opt[storeStart].mlen,
                 opt[storeStart].off);
        if (nextStretch.mlen == 0) {

          break;
        }
        storeStart--;
        opt[storeStart] = nextStretch;
        assert(nextStretch.litlen + nextStretch.mlen <= stretchPos);
        stretchPos -= nextStretch.litlen + nextStretch.mlen;
      }

      DEBUGLOG(6, "sending selected sequences into seqStore");
      {
        U32 storePos;
        for (storePos = storeStart; storePos <= storeEnd; storePos++) {
          U32 const llen = opt[storePos].litlen;
          U32 const mlen = opt[storePos].mlen;
          U32 const offBase = opt[storePos].off;
          U32 const advance = llen + mlen;
          DEBUGLOG(6, "considering seq starting at %i, llen=%u, mlen=%u",
                   (int)(anchor - istart), (unsigned)llen, (unsigned)mlen);

          if (mlen == 0) {

            assert(storePos == storeEnd);
            ip = anchor + llen;

            continue;
          }

          assert(anchor + llen <= iend);
          ZSTD_updateStats(optStatePtr, llen, anchor, offBase, mlen);
          ZSTD_storeSeq(seqStore, llen, anchor, iend, offBase, mlen);
          anchor += advance;
          ip = anchor;
        }
      }
      DEBUGLOG(7, "new offset history : %u, %u, %u", rep[0], rep[1], rep[2]);

      ZSTD_setBasePrices(optStatePtr, optLevel);
    }
  }

  return (size_t)(iend - anchor);
}
#endif

#ifndef ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
static size_t ZSTD_compressBlock_opt0(ZSTD_MatchState_t *ms,
                                      SeqStore_t *seqStore,
                                      U32 rep[ZSTD_REP_NUM], const void *src,
                                      size_t srcSize,
                                      const ZSTD_dictMode_e dictMode) {
  return ZSTD_compressBlock_opt_generic(ms, seqStore, rep, src, srcSize, 0,
                                        dictMode);
}
#endif

#ifndef ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR
static size_t ZSTD_compressBlock_opt2(ZSTD_MatchState_t *ms,
                                      SeqStore_t *seqStore,
                                      U32 rep[ZSTD_REP_NUM], const void *src,
                                      size_t srcSize,
                                      const ZSTD_dictMode_e dictMode) {
  return ZSTD_compressBlock_opt_generic(ms, seqStore, rep, src, srcSize, 2,
                                        dictMode);
}
#endif

#ifndef ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
size_t ZSTD_compressBlock_btopt(ZSTD_MatchState_t *ms, SeqStore_t *seqStore,
                                U32 rep[ZSTD_REP_NUM], const void *src,
                                size_t srcSize) {
  DEBUGLOG(5, "ZSTD_compressBlock_btopt");
  return ZSTD_compressBlock_opt0(ms, seqStore, rep, src, srcSize, ZSTD_noDict);
}
#endif

#ifndef ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR void
ZSTD_initStats_ultra(ZSTD_MatchState_t *ms, SeqStore_t *seqStore,
                     U32 rep[ZSTD_REP_NUM], const void *src, size_t srcSize) {
  U32 tmpRep[ZSTD_REP_NUM];
  ZSTD_memcpy(tmpRep, rep, sizeof(tmpRep));

  DEBUGLOG(4, "ZSTD_initStats_ultra (srcSize=%zu)", srcSize);
  assert(ms->opt.litLengthSum == 0);
  assert(seqStore->sequences == seqStore->sequencesStart);
  assert(ms->window.dictLimit == ms->window.lowLimit);
  assert(ms->window.dictLimit - ms->nextToUpdate <= 1);

  ZSTD_compressBlock_opt2(ms, seqStore, tmpRep, src, srcSize, ZSTD_noDict);

  ZSTD_resetSeqStore(seqStore);
  ms->window.base -= srcSize;
  ms->window.dictLimit += (U32)srcSize;
  ms->window.lowLimit = ms->window.dictLimit;
  ms->nextToUpdate = ms->window.dictLimit;
}

size_t ZSTD_compressBlock_btultra(ZSTD_MatchState_t *ms, SeqStore_t *seqStore,
                                  U32 rep[ZSTD_REP_NUM], const void *src,
                                  size_t srcSize) {
  DEBUGLOG(5, "ZSTD_compressBlock_btultra (srcSize=%zu)", srcSize);
  return ZSTD_compressBlock_opt2(ms, seqStore, rep, src, srcSize, ZSTD_noDict);
}

size_t ZSTD_compressBlock_btultra2(ZSTD_MatchState_t *ms, SeqStore_t *seqStore,
                                   U32 rep[ZSTD_REP_NUM], const void *src,
                                   size_t srcSize) {
  U32 const curr = (U32)((const BYTE *)src - ms->window.base);
  DEBUGLOG(5, "ZSTD_compressBlock_btultra2 (srcSize=%zu)", srcSize);

  assert(srcSize <= ZSTD_BLOCKSIZE_MAX);
  if ((ms->opt.litLengthSum == 0) &&
      (seqStore->sequences == seqStore->sequencesStart) &&
      (ms->window.dictLimit == ms->window.lowLimit) &&
      (curr == ms->window.dictLimit)

      && (srcSize > ZSTD_PREDEF_THRESHOLD)

  ) {
    ZSTD_initStats_ultra(ms, seqStore, rep, src, srcSize);
  }

  return ZSTD_compressBlock_opt2(ms, seqStore, rep, src, srcSize, ZSTD_noDict);
}
#endif

#ifndef ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
size_t ZSTD_compressBlock_btopt_dictMatchState(ZSTD_MatchState_t *ms,
                                               SeqStore_t *seqStore,
                                               U32 rep[ZSTD_REP_NUM],
                                               const void *src,
                                               size_t srcSize) {
  return ZSTD_compressBlock_opt0(ms, seqStore, rep, src, srcSize,
                                 ZSTD_dictMatchState);
}

size_t ZSTD_compressBlock_btopt_extDict(ZSTD_MatchState_t *ms,
                                        SeqStore_t *seqStore,
                                        U32 rep[ZSTD_REP_NUM], const void *src,
                                        size_t srcSize) {
  return ZSTD_compressBlock_opt0(ms, seqStore, rep, src, srcSize, ZSTD_extDict);
}
#endif

#ifndef ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR
size_t ZSTD_compressBlock_btultra_dictMatchState(ZSTD_MatchState_t *ms,
                                                 SeqStore_t *seqStore,
                                                 U32 rep[ZSTD_REP_NUM],
                                                 const void *src,
                                                 size_t srcSize) {
  return ZSTD_compressBlock_opt2(ms, seqStore, rep, src, srcSize,
                                 ZSTD_dictMatchState);
}

size_t ZSTD_compressBlock_btultra_extDict(ZSTD_MatchState_t *ms,
                                          SeqStore_t *seqStore,
                                          U32 rep[ZSTD_REP_NUM],
                                          const void *src, size_t srcSize) {
  return ZSTD_compressBlock_opt2(ms, seqStore, rep, src, srcSize, ZSTD_extDict);
}
#endif
