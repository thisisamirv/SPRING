/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "zstd_fast.h"
#include "zstd_compress_internal.h"

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR void
ZSTD_fillHashTableForCDict(ZSTD_MatchState_t *ms, const void *const end,
                           ZSTD_dictTableLoadMethod_e dtlm) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hBits = cParams->hashLog + ZSTD_SHORT_CACHE_TAG_BITS;
  U32 const mls = cParams->minMatch;
  const BYTE *const base = ms->window.base;
  const BYTE *ip = base + ms->nextToUpdate;
  const BYTE *const iend = ((const BYTE *)end) - HASH_READ_SIZE;
  const U32 fastHashFillStep = 3;

  assert(dtlm == ZSTD_dtlm_full);

  for (; ip + fastHashFillStep < iend + 2; ip += fastHashFillStep) {
    U32 const curr = (U32)(ip - base);
    {
      size_t const hashAndTag = ZSTD_hashPtr(ip, hBits, mls);
      ZSTD_writeTaggedIndex(hashTable, hashAndTag, curr);
    }

    if (dtlm == ZSTD_dtlm_fast)
      continue;

    {
      U32 p;
      for (p = 1; p < fastHashFillStep; ++p) {
        size_t const hashAndTag = ZSTD_hashPtr(ip + p, hBits, mls);
        if (hashTable[hashAndTag >> ZSTD_SHORT_CACHE_TAG_BITS] == 0) {
          ZSTD_writeTaggedIndex(hashTable, hashAndTag, curr + p);
        }
      }
    }
  }
}

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR void
ZSTD_fillHashTableForCCtx(ZSTD_MatchState_t *ms, const void *const end,
                          ZSTD_dictTableLoadMethod_e dtlm) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hBits = cParams->hashLog;
  U32 const mls = cParams->minMatch;
  const BYTE *const base = ms->window.base;
  const BYTE *ip = base + ms->nextToUpdate;
  const BYTE *const iend = ((const BYTE *)end) - HASH_READ_SIZE;
  const U32 fastHashFillStep = 3;

  assert(dtlm == ZSTD_dtlm_fast);

  for (; ip + fastHashFillStep < iend + 2; ip += fastHashFillStep) {
    U32 const curr = (U32)(ip - base);
    size_t const hash0 = ZSTD_hashPtr(ip, hBits, mls);
    hashTable[hash0] = curr;
    if (dtlm == ZSTD_dtlm_fast)
      continue;

    {
      U32 p;
      for (p = 1; p < fastHashFillStep; ++p) {
        size_t const hash = ZSTD_hashPtr(ip + p, hBits, mls);
        if (hashTable[hash] == 0) {
          hashTable[hash] = curr + p;
        }
      }
    }
  }
}

void ZSTD_fillHashTable(ZSTD_MatchState_t *ms, const void *const end,
                        ZSTD_dictTableLoadMethod_e dtlm,
                        ZSTD_tableFillPurpose_e tfp) {
  if (tfp == ZSTD_tfp_forCDict) {
    ZSTD_fillHashTableForCDict(ms, end, dtlm);
  } else {
    ZSTD_fillHashTableForCCtx(ms, end, dtlm);
  }
}

typedef int (*ZSTD_match4Found)(const BYTE *currentPtr,
                                const BYTE *matchAddress, U32 matchIdx,
                                U32 idxLowLimit);

static int ZSTD_match4Found_cmov(const BYTE *currentPtr,
                                 const BYTE *matchAddress, U32 matchIdx,
                                 U32 idxLowLimit) {

  static const BYTE dummy[] = {0x12, 0x34, 0x56, 0x78};

  const BYTE *mvalAddr =
      ZSTD_selectAddr(matchIdx, idxLowLimit, matchAddress, dummy);

  if (MEM_read32(currentPtr) != MEM_read32(mvalAddr))
    return 0;

#if defined(__GNUC__)
  __asm__("");
#endif
  return matchIdx >= idxLowLimit;
}

static int ZSTD_match4Found_branch(const BYTE *currentPtr,
                                   const BYTE *matchAddress, U32 matchIdx,
                                   U32 idxLowLimit) {

  U32 mval;
  if (matchIdx >= idxLowLimit) {
    mval = MEM_read32(matchAddress);
  } else {
    mval = MEM_read32(currentPtr) ^ 1;
  }

  return (MEM_read32(currentPtr) == mval);
}

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
size_t ZSTD_compressBlock_fast_noDict_generic(ZSTD_MatchState_t *ms,
                                              SeqStore_t *seqStore,
                                              U32 rep[ZSTD_REP_NUM],
                                              void const *src, size_t srcSize,
                                              U32 const mls, int useCmov) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hlog = cParams->hashLog;
  size_t const stepSize = cParams->targetLength + !(cParams->targetLength) + 1;
  const BYTE *const base = ms->window.base;
  const BYTE *const istart = (const BYTE *)src;
  const U32 endIndex = (U32)((size_t)(istart - base) + srcSize);
  const U32 prefixStartIndex =
      ZSTD_getLowestPrefixIndex(ms, endIndex, cParams->windowLog);
  const BYTE *const prefixStart = base + prefixStartIndex;
  const BYTE *const iend = istart + srcSize;
  const BYTE *const ilimit = iend - HASH_READ_SIZE;

  const BYTE *anchor = istart;
  const BYTE *ip0 = istart;
  const BYTE *ip1;
  const BYTE *ip2;
  const BYTE *ip3;
  U32 current0;

  U32 rep_offset1 = rep[0];
  U32 rep_offset2 = rep[1];
  U32 offsetSaved1 = 0, offsetSaved2 = 0;

  size_t hash0;
  size_t hash1;
  U32 matchIdx;

  U32 offcode;
  const BYTE *match0;
  size_t mLength;

  size_t step;
  const BYTE *nextStep;
  const size_t kStepIncr = (1 << (kSearchStrength - 1));
  const ZSTD_match4Found matchFound =
      useCmov ? ZSTD_match4Found_cmov : ZSTD_match4Found_branch;

  DEBUGLOG(5, "ZSTD_compressBlock_fast_generic");
  ip0 += (ip0 == prefixStart);
  {
    U32 const curr = (U32)(ip0 - base);
    U32 const windowLow =
        ZSTD_getLowestPrefixIndex(ms, curr, cParams->windowLog);
    U32 const maxRep = curr - windowLow;
    if (rep_offset2 > maxRep)
      offsetSaved2 = rep_offset2, rep_offset2 = 0;
    if (rep_offset1 > maxRep)
      offsetSaved1 = rep_offset1, rep_offset1 = 0;
  }

_start:

  step = stepSize;
  nextStep = ip0 + kStepIncr;

  ip1 = ip0 + 1;
  ip2 = ip0 + step;
  ip3 = ip2 + 1;

  if (ip3 >= ilimit) {
    goto _cleanup;
  }

  hash0 = ZSTD_hashPtr(ip0, hlog, mls);
  hash1 = ZSTD_hashPtr(ip1, hlog, mls);

  matchIdx = hashTable[hash0];

  do {

    const U32 rval = MEM_read32(ip2 - rep_offset1);

    current0 = (U32)(ip0 - base);
    hashTable[hash0] = current0;

    if ((MEM_read32(ip2) == rval) & (rep_offset1 > 0)) {
      ip0 = ip2;
      match0 = ip0 - rep_offset1;
      mLength = ip0[-1] == match0[-1];
      ip0 -= mLength;
      match0 -= mLength;
      offcode = REPCODE1_TO_OFFBASE;
      mLength += 4;

      hashTable[hash1] = (U32)(ip1 - base);

      goto _match;
    }

    if (matchFound(ip0, base + matchIdx, matchIdx, prefixStartIndex)) {

      hashTable[hash1] = (U32)(ip1 - base);

      goto _offset;
    }

    matchIdx = hashTable[hash1];

    hash0 = hash1;
    hash1 = ZSTD_hashPtr(ip2, hlog, mls);

    ip0 = ip1;
    ip1 = ip2;
    ip2 = ip3;

    current0 = (U32)(ip0 - base);
    hashTable[hash0] = current0;

    if (matchFound(ip0, base + matchIdx, matchIdx, prefixStartIndex)) {

      if (step <= 4) {

        hashTable[hash1] = (U32)(ip1 - base);
      }
      goto _offset;
    }

    matchIdx = hashTable[hash1];

    hash0 = hash1;
    hash1 = ZSTD_hashPtr(ip2, hlog, mls);

    ip0 = ip1;
    ip1 = ip2;
    ip2 = ip0 + step;
    ip3 = ip1 + step;

    if (ip2 >= nextStep) {
      step++;
      PREFETCH_L1(ip1 + 64);
      PREFETCH_L1(ip1 + 128);
      nextStep += kStepIncr;
    }
  } while (ip3 < ilimit);

_cleanup:

  offsetSaved2 =
      ((offsetSaved1 != 0) && (rep_offset1 != 0)) ? offsetSaved1 : offsetSaved2;

  rep[0] = rep_offset1 ? rep_offset1 : offsetSaved1;
  rep[1] = rep_offset2 ? rep_offset2 : offsetSaved2;

  return (size_t)(iend - anchor);

_offset:

  match0 = base + matchIdx;
  rep_offset2 = rep_offset1;
  rep_offset1 = (U32)(ip0 - match0);
  offcode = OFFSET_TO_OFFBASE(rep_offset1);
  mLength = 4;

  while (((ip0 > anchor) & (match0 > prefixStart)) && (ip0[-1] == match0[-1])) {
    ip0--;
    match0--;
    mLength++;
  }

_match:

  mLength += ZSTD_count(ip0 + mLength, match0 + mLength, iend);

  ZSTD_storeSeq(seqStore, (size_t)(ip0 - anchor), anchor, iend, offcode,
                mLength);

  ip0 += mLength;
  anchor = ip0;

  if (ip0 <= ilimit) {

    assert(base + current0 + 2 > istart);
    hashTable[ZSTD_hashPtr(base + current0 + 2, hlog, mls)] = current0 + 2;
    hashTable[ZSTD_hashPtr(ip0 - 2, hlog, mls)] = (U32)(ip0 - 2 - base);

    if (rep_offset2 > 0) {
      while ((ip0 <= ilimit) &&
             (MEM_read32(ip0) == MEM_read32(ip0 - rep_offset2))) {

        size_t const rLength =
            ZSTD_count(ip0 + 4, ip0 + 4 - rep_offset2, iend) + 4;
        {
          U32 const tmpOff = rep_offset2;
          rep_offset2 = rep_offset1;
          rep_offset1 = tmpOff;
        }
        hashTable[ZSTD_hashPtr(ip0, hlog, mls)] = (U32)(ip0 - base);
        ip0 += rLength;
        ZSTD_storeSeq(seqStore, 0, anchor, iend, REPCODE1_TO_OFFBASE, rLength);
        anchor = ip0;
        continue;
      }
    }
  }

  goto _start;
}

#define ZSTD_GEN_FAST_FN(dictMode, mml, cmov)                                  \
  static size_t ZSTD_compressBlock_fast_##dictMode##_##mml##_##cmov(           \
      ZSTD_MatchState_t *ms, SeqStore_t *seqStore, U32 rep[ZSTD_REP_NUM],      \
      void const *src, size_t srcSize) {                                       \
    return ZSTD_compressBlock_fast_##dictMode##_generic(                       \
        ms, seqStore, rep, src, srcSize, mml, cmov);                           \
  }

ZSTD_GEN_FAST_FN(noDict, 4, 1)
ZSTD_GEN_FAST_FN(noDict, 5, 1)
ZSTD_GEN_FAST_FN(noDict, 6, 1)
ZSTD_GEN_FAST_FN(noDict, 7, 1)

ZSTD_GEN_FAST_FN(noDict, 4, 0)
ZSTD_GEN_FAST_FN(noDict, 5, 0)
ZSTD_GEN_FAST_FN(noDict, 6, 0)
ZSTD_GEN_FAST_FN(noDict, 7, 0)

size_t ZSTD_compressBlock_fast(ZSTD_MatchState_t *ms, SeqStore_t *seqStore,
                               U32 rep[ZSTD_REP_NUM], void const *src,
                               size_t srcSize) {
  U32 const mml = ms->cParams.minMatch;

  int const useCmov = ms->cParams.windowLog < 19;
  assert(ms->dictMatchState == NULL);
  if (useCmov) {
    switch (mml) {
    default:
    case 4:
      return ZSTD_compressBlock_fast_noDict_4_1(ms, seqStore, rep, src,
                                                srcSize);
    case 5:
      return ZSTD_compressBlock_fast_noDict_5_1(ms, seqStore, rep, src,
                                                srcSize);
    case 6:
      return ZSTD_compressBlock_fast_noDict_6_1(ms, seqStore, rep, src,
                                                srcSize);
    case 7:
      return ZSTD_compressBlock_fast_noDict_7_1(ms, seqStore, rep, src,
                                                srcSize);
    }
  } else {

    switch (mml) {
    default:
    case 4:
      return ZSTD_compressBlock_fast_noDict_4_0(ms, seqStore, rep, src,
                                                srcSize);
    case 5:
      return ZSTD_compressBlock_fast_noDict_5_0(ms, seqStore, rep, src,
                                                srcSize);
    case 6:
      return ZSTD_compressBlock_fast_noDict_6_0(ms, seqStore, rep, src,
                                                srcSize);
    case 7:
      return ZSTD_compressBlock_fast_noDict_7_0(ms, seqStore, rep, src,
                                                srcSize);
    }
  }
}

FORCE_INLINE_TEMPLATE
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
size_t ZSTD_compressBlock_fast_dictMatchState_generic(
    ZSTD_MatchState_t *ms, SeqStore_t *seqStore, U32 rep[ZSTD_REP_NUM],
    void const *src, size_t srcSize, U32 const mls, U32 const hasStep) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hlog = cParams->hashLog;

  U32 const stepSize = cParams->targetLength + !(cParams->targetLength);
  const BYTE *const base = ms->window.base;
  const BYTE *const istart = (const BYTE *)src;
  const BYTE *ip0 = istart;
  const BYTE *ip1 = ip0 + stepSize;
  const BYTE *anchor = istart;
  const U32 prefixStartIndex = ms->window.dictLimit;
  const BYTE *const prefixStart = base + prefixStartIndex;
  const BYTE *const iend = istart + srcSize;
  const BYTE *const ilimit = iend - HASH_READ_SIZE;
  U32 offset_1 = rep[0], offset_2 = rep[1];

  const ZSTD_MatchState_t *const dms = ms->dictMatchState;
  const ZSTD_compressionParameters *const dictCParams = &dms->cParams;
  const U32 *const dictHashTable = dms->hashTable;
  const U32 dictStartIndex = dms->window.dictLimit;
  const BYTE *const dictBase = dms->window.base;
  const BYTE *const dictStart = dictBase + dictStartIndex;
  const BYTE *const dictEnd = dms->window.nextSrc;
  const U32 dictIndexDelta = prefixStartIndex - (U32)(dictEnd - dictBase);
  const U32 dictAndPrefixLength =
      (U32)(istart - prefixStart + dictEnd - dictStart);
  const U32 dictHBits = dictCParams->hashLog + ZSTD_SHORT_CACHE_TAG_BITS;

  const U32 maxDistance = 1U << cParams->windowLog;
  const U32 endIndex = (U32)((size_t)(istart - base) + srcSize);
  assert(endIndex - prefixStartIndex <= maxDistance);
  (void)maxDistance;
  (void)endIndex;

  (void)hasStep;

  assert(prefixStartIndex >= (U32)(dictEnd - dictBase));

  if (ms->prefetchCDictTables) {
    size_t const hashTableBytes =
        (((size_t)1) << dictCParams->hashLog) * sizeof(U32);
    PREFETCH_AREA(dictHashTable, hashTableBytes);
  }

  DEBUGLOG(5, "ZSTD_compressBlock_fast_dictMatchState_generic");
  ip0 += (dictAndPrefixLength == 0);

  assert(offset_1 <= dictAndPrefixLength);
  assert(offset_2 <= dictAndPrefixLength);

  assert(stepSize >= 1);
  while (ip1 <= ilimit) {
    size_t mLength;
    size_t hash0 = ZSTD_hashPtr(ip0, hlog, mls);

    size_t const dictHashAndTag0 = ZSTD_hashPtr(ip0, dictHBits, mls);
    U32 dictMatchIndexAndTag =
        dictHashTable[dictHashAndTag0 >> ZSTD_SHORT_CACHE_TAG_BITS];
    int dictTagsMatch =
        ZSTD_comparePackedTags(dictMatchIndexAndTag, dictHashAndTag0);

    U32 matchIndex = hashTable[hash0];
    U32 curr = (U32)(ip0 - base);
    size_t step = stepSize;
    const size_t kStepIncr = 1 << kSearchStrength;
    const BYTE *nextStep = ip0 + kStepIncr;

    while (1) {
      const BYTE *match = base + matchIndex;
      const U32 repIndex = curr + 1 - offset_1;
      const BYTE *repMatch = (repIndex < prefixStartIndex)
                                 ? dictBase + (repIndex - dictIndexDelta)
                                 : base + repIndex;
      const size_t hash1 = ZSTD_hashPtr(ip1, hlog, mls);
      size_t const dictHashAndTag1 = ZSTD_hashPtr(ip1, dictHBits, mls);
      hashTable[hash0] = curr;

      if ((ZSTD_index_overlap_check(prefixStartIndex, repIndex)) &&
          (MEM_read32(repMatch) == MEM_read32(ip0 + 1))) {
        const BYTE *const repMatchEnd =
            repIndex < prefixStartIndex ? dictEnd : iend;
        mLength = ZSTD_count_2segments(ip0 + 1 + 4, repMatch + 4, iend,
                                       repMatchEnd, prefixStart) +
                  4;
        ip0++;
        ZSTD_storeSeq(seqStore, (size_t)(ip0 - anchor), anchor, iend,
                      REPCODE1_TO_OFFBASE, mLength);
        break;
      }

      if (dictTagsMatch) {

        const U32 dictMatchIndex =
            dictMatchIndexAndTag >> ZSTD_SHORT_CACHE_TAG_BITS;
        const BYTE *dictMatch = dictBase + dictMatchIndex;
        if (dictMatchIndex > dictStartIndex &&
            MEM_read32(dictMatch) == MEM_read32(ip0)) {

          if (matchIndex <= prefixStartIndex) {
            U32 const offset = (U32)(curr - dictMatchIndex - dictIndexDelta);
            mLength = ZSTD_count_2segments(ip0 + 4, dictMatch + 4, iend,
                                           dictEnd, prefixStart) +
                      4;
            while (((ip0 > anchor) & (dictMatch > dictStart)) &&
                   (ip0[-1] == dictMatch[-1])) {
              ip0--;
              dictMatch--;
              mLength++;
            }
            offset_2 = offset_1;
            offset_1 = offset;
            ZSTD_storeSeq(seqStore, (size_t)(ip0 - anchor), anchor, iend,
                          OFFSET_TO_OFFBASE(offset), mLength);
            break;
          }
        }
      }

      if (ZSTD_match4Found_cmov(ip0, match, matchIndex, prefixStartIndex)) {

        U32 const offset = (U32)(ip0 - match);
        mLength = ZSTD_count(ip0 + 4, match + 4, iend) + 4;
        while (((ip0 > anchor) & (match > prefixStart)) &&
               (ip0[-1] == match[-1])) {
          ip0--;
          match--;
          mLength++;
        }
        offset_2 = offset_1;
        offset_1 = offset;
        ZSTD_storeSeq(seqStore, (size_t)(ip0 - anchor), anchor, iend,
                      OFFSET_TO_OFFBASE(offset), mLength);
        break;
      }

      dictMatchIndexAndTag =
          dictHashTable[dictHashAndTag1 >> ZSTD_SHORT_CACHE_TAG_BITS];
      dictTagsMatch =
          ZSTD_comparePackedTags(dictMatchIndexAndTag, dictHashAndTag1);
      matchIndex = hashTable[hash1];

      if (ip1 >= nextStep) {
        step++;
        nextStep += kStepIncr;
      }
      ip0 = ip1;
      ip1 = ip1 + step;
      if (ip1 > ilimit)
        goto _cleanup;

      curr = (U32)(ip0 - base);
      hash0 = hash1;
    }

    assert(mLength);
    ip0 += mLength;
    anchor = ip0;

    if (ip0 <= ilimit) {

      assert(base + curr + 2 > istart);
      hashTable[ZSTD_hashPtr(base + curr + 2, hlog, mls)] = curr + 2;
      hashTable[ZSTD_hashPtr(ip0 - 2, hlog, mls)] = (U32)(ip0 - 2 - base);

      while (ip0 <= ilimit) {
        U32 const current2 = (U32)(ip0 - base);
        U32 const repIndex2 = current2 - offset_2;
        const BYTE *repMatch2 = repIndex2 < prefixStartIndex
                                    ? dictBase - dictIndexDelta + repIndex2
                                    : base + repIndex2;
        if ((ZSTD_index_overlap_check(prefixStartIndex, repIndex2)) &&
            (MEM_read32(repMatch2) == MEM_read32(ip0))) {
          const BYTE *const repEnd2 =
              repIndex2 < prefixStartIndex ? dictEnd : iend;
          size_t const repLength2 =
              ZSTD_count_2segments(ip0 + 4, repMatch2 + 4, iend, repEnd2,
                                   prefixStart) +
              4;
          U32 tmpOffset = offset_2;
          offset_2 = offset_1;
          offset_1 = tmpOffset;
          ZSTD_storeSeq(seqStore, 0, anchor, iend, REPCODE1_TO_OFFBASE,
                        repLength2);
          hashTable[ZSTD_hashPtr(ip0, hlog, mls)] = current2;
          ip0 += repLength2;
          anchor = ip0;
          continue;
        }
        break;
      }
    }

    assert(ip0 == anchor);
    ip1 = ip0 + stepSize;
  }

_cleanup:

  rep[0] = offset_1;
  rep[1] = offset_2;

  return (size_t)(iend - anchor);
}

ZSTD_GEN_FAST_FN(dictMatchState, 4, 0)
ZSTD_GEN_FAST_FN(dictMatchState, 5, 0)
ZSTD_GEN_FAST_FN(dictMatchState, 6, 0)
ZSTD_GEN_FAST_FN(dictMatchState, 7, 0)

size_t ZSTD_compressBlock_fast_dictMatchState(ZSTD_MatchState_t *ms,
                                              SeqStore_t *seqStore,
                                              U32 rep[ZSTD_REP_NUM],
                                              void const *src, size_t srcSize) {
  U32 const mls = ms->cParams.minMatch;
  assert(ms->dictMatchState != NULL);
  switch (mls) {
  default:
  case 4:
    return ZSTD_compressBlock_fast_dictMatchState_4_0(ms, seqStore, rep, src,
                                                      srcSize);
  case 5:
    return ZSTD_compressBlock_fast_dictMatchState_5_0(ms, seqStore, rep, src,
                                                      srcSize);
  case 6:
    return ZSTD_compressBlock_fast_dictMatchState_6_0(ms, seqStore, rep, src,
                                                      srcSize);
  case 7:
    return ZSTD_compressBlock_fast_dictMatchState_7_0(ms, seqStore, rep, src,
                                                      srcSize);
  }
}

static ZSTD_ALLOW_POINTER_OVERFLOW_ATTR size_t
ZSTD_compressBlock_fast_extDict_generic(ZSTD_MatchState_t *ms,
                                        SeqStore_t *seqStore,
                                        U32 rep[ZSTD_REP_NUM], void const *src,
                                        size_t srcSize, U32 const mls,
                                        U32 const hasStep) {
  const ZSTD_compressionParameters *const cParams = &ms->cParams;
  U32 *const hashTable = ms->hashTable;
  U32 const hlog = cParams->hashLog;

  size_t const stepSize = cParams->targetLength + !(cParams->targetLength) + 1;
  const BYTE *const base = ms->window.base;
  const BYTE *const dictBase = ms->window.dictBase;
  const BYTE *const istart = (const BYTE *)src;
  const BYTE *anchor = istart;
  const U32 endIndex = (U32)((size_t)(istart - base) + srcSize);
  const U32 lowLimit =
      ZSTD_getLowestMatchIndex(ms, endIndex, cParams->windowLog);
  const U32 dictStartIndex = lowLimit;
  const BYTE *const dictStart = dictBase + dictStartIndex;
  const U32 dictLimit = ms->window.dictLimit;
  const U32 prefixStartIndex = dictLimit < lowLimit ? lowLimit : dictLimit;
  const BYTE *const prefixStart = base + prefixStartIndex;
  const BYTE *const dictEnd = dictBase + prefixStartIndex;
  const BYTE *const iend = istart + srcSize;
  const BYTE *const ilimit = iend - 8;
  U32 offset_1 = rep[0], offset_2 = rep[1];
  U32 offsetSaved1 = 0, offsetSaved2 = 0;

  const BYTE *ip0 = istart;
  const BYTE *ip1;
  const BYTE *ip2;
  const BYTE *ip3;
  U32 current0;

  size_t hash0;
  size_t hash1;
  U32 idx;
  const BYTE *idxBase;

  U32 offcode;
  const BYTE *match0;
  size_t mLength;
  const BYTE *matchEnd = 0;

  size_t step;
  const BYTE *nextStep;
  const size_t kStepIncr = (1 << (kSearchStrength - 1));

  (void)hasStep;

  DEBUGLOG(5, "ZSTD_compressBlock_fast_extDict_generic (offset_1=%u)",
           offset_1);

  if (prefixStartIndex == dictStartIndex)
    return ZSTD_compressBlock_fast(ms, seqStore, rep, src, srcSize);

  {
    U32 const curr = (U32)(ip0 - base);
    U32 const maxRep = curr - dictStartIndex;
    if (offset_2 >= maxRep)
      offsetSaved2 = offset_2, offset_2 = 0;
    if (offset_1 >= maxRep)
      offsetSaved1 = offset_1, offset_1 = 0;
  }

_start:

  step = stepSize;
  nextStep = ip0 + kStepIncr;

  ip1 = ip0 + 1;
  ip2 = ip0 + step;
  ip3 = ip2 + 1;

  if (ip3 >= ilimit) {
    goto _cleanup;
  }

  hash0 = ZSTD_hashPtr(ip0, hlog, mls);
  hash1 = ZSTD_hashPtr(ip1, hlog, mls);

  idx = hashTable[hash0];
  idxBase = idx < prefixStartIndex ? dictBase : base;

  do {
    {
      U32 const current2 = (U32)(ip2 - base);
      U32 const repIndex = current2 - offset_1;
      const BYTE *const repBase = repIndex < prefixStartIndex ? dictBase : base;
      U32 rval;
      if (((U32)(prefixStartIndex - repIndex) >= 4) & (offset_1 > 0)) {
        rval = MEM_read32(repBase + repIndex);
      } else {
        rval = MEM_read32(ip2) ^ 1;
      }

      current0 = (U32)(ip0 - base);
      hashTable[hash0] = current0;

      if (MEM_read32(ip2) == rval) {
        ip0 = ip2;
        match0 = repBase + repIndex;
        matchEnd = repIndex < prefixStartIndex ? dictEnd : iend;
        assert((match0 != prefixStart) & (match0 != dictStart));
        mLength = ip0[-1] == match0[-1];
        ip0 -= mLength;
        match0 -= mLength;
        offcode = REPCODE1_TO_OFFBASE;
        mLength += 4;
        goto _match;
      }
    }

    {
      U32 const mval = idx >= dictStartIndex ? MEM_read32(idxBase + idx)
                                             : MEM_read32(ip0) ^ 1;

      if (MEM_read32(ip0) == mval) {

        goto _offset;
      }
    }

    idx = hashTable[hash1];
    idxBase = idx < prefixStartIndex ? dictBase : base;

    hash0 = hash1;
    hash1 = ZSTD_hashPtr(ip2, hlog, mls);

    ip0 = ip1;
    ip1 = ip2;
    ip2 = ip3;

    current0 = (U32)(ip0 - base);
    hashTable[hash0] = current0;

    {
      U32 const mval = idx >= dictStartIndex ? MEM_read32(idxBase + idx)
                                             : MEM_read32(ip0) ^ 1;

      if (MEM_read32(ip0) == mval) {

        goto _offset;
      }
    }

    idx = hashTable[hash1];
    idxBase = idx < prefixStartIndex ? dictBase : base;

    hash0 = hash1;
    hash1 = ZSTD_hashPtr(ip2, hlog, mls);

    ip0 = ip1;
    ip1 = ip2;
    ip2 = ip0 + step;
    ip3 = ip1 + step;

    if (ip2 >= nextStep) {
      step++;
      PREFETCH_L1(ip1 + 64);
      PREFETCH_L1(ip1 + 128);
      nextStep += kStepIncr;
    }
  } while (ip3 < ilimit);

_cleanup:

  offsetSaved2 =
      ((offsetSaved1 != 0) && (offset_1 != 0)) ? offsetSaved1 : offsetSaved2;

  rep[0] = offset_1 ? offset_1 : offsetSaved1;
  rep[1] = offset_2 ? offset_2 : offsetSaved2;

  return (size_t)(iend - anchor);

_offset:

{
  U32 const offset = current0 - idx;
  const BYTE *const lowMatchPtr =
      idx < prefixStartIndex ? dictStart : prefixStart;
  matchEnd = idx < prefixStartIndex ? dictEnd : iend;
  match0 = idxBase + idx;
  offset_2 = offset_1;
  offset_1 = offset;
  offcode = OFFSET_TO_OFFBASE(offset);
  mLength = 4;

  while (((ip0 > anchor) & (match0 > lowMatchPtr)) && (ip0[-1] == match0[-1])) {
    ip0--;
    match0--;
    mLength++;
  }
}

_match:

  assert(matchEnd != 0);
  mLength += ZSTD_count_2segments(ip0 + mLength, match0 + mLength, iend,
                                  matchEnd, prefixStart);

  ZSTD_storeSeq(seqStore, (size_t)(ip0 - anchor), anchor, iend, offcode,
                mLength);

  ip0 += mLength;
  anchor = ip0;

  if (ip1 < ip0) {
    hashTable[hash1] = (U32)(ip1 - base);
  }

  if (ip0 <= ilimit) {

    assert(base + current0 + 2 > istart);
    hashTable[ZSTD_hashPtr(base + current0 + 2, hlog, mls)] = current0 + 2;
    hashTable[ZSTD_hashPtr(ip0 - 2, hlog, mls)] = (U32)(ip0 - 2 - base);

    while (ip0 <= ilimit) {
      U32 const repIndex2 = (U32)(ip0 - base) - offset_2;
      const BYTE *const repMatch2 = repIndex2 < prefixStartIndex
                                        ? dictBase + repIndex2
                                        : base + repIndex2;
      if (((ZSTD_index_overlap_check(prefixStartIndex, repIndex2)) &
           (offset_2 > 0)) &&
          (MEM_read32(repMatch2) == MEM_read32(ip0))) {
        const BYTE *const repEnd2 =
            repIndex2 < prefixStartIndex ? dictEnd : iend;
        size_t const repLength2 =
            ZSTD_count_2segments(ip0 + 4, repMatch2 + 4, iend, repEnd2,
                                 prefixStart) +
            4;
        {
          U32 const tmpOffset = offset_2;
          offset_2 = offset_1;
          offset_1 = tmpOffset;
        }
        ZSTD_storeSeq(seqStore, 0, anchor, iend, REPCODE1_TO_OFFBASE,
                      repLength2);
        hashTable[ZSTD_hashPtr(ip0, hlog, mls)] = (U32)(ip0 - base);
        ip0 += repLength2;
        anchor = ip0;
        continue;
      }
      break;
    }
  }

  goto _start;
}

ZSTD_GEN_FAST_FN(extDict, 4, 0)
ZSTD_GEN_FAST_FN(extDict, 5, 0)
ZSTD_GEN_FAST_FN(extDict, 6, 0)
ZSTD_GEN_FAST_FN(extDict, 7, 0)

size_t ZSTD_compressBlock_fast_extDict(ZSTD_MatchState_t *ms,
                                       SeqStore_t *seqStore,
                                       U32 rep[ZSTD_REP_NUM], void const *src,
                                       size_t srcSize) {
  U32 const mls = ms->cParams.minMatch;
  assert(ms->dictMatchState == NULL);
  switch (mls) {
  default:
  case 4:
    return ZSTD_compressBlock_fast_extDict_4_0(ms, seqStore, rep, src, srcSize);
  case 5:
    return ZSTD_compressBlock_fast_extDict_5_0(ms, seqStore, rep, src, srcSize);
  case 6:
    return ZSTD_compressBlock_fast_extDict_6_0(ms, seqStore, rep, src, srcSize);
  case 7:
    return ZSTD_compressBlock_fast_extDict_7_0(ms, seqStore, rep, src, srcSize);
  }
}
