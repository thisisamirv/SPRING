/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_LDM_H
#define ZSTD_LDM_H

#include "zstd.h"
#include "zstd_compress_internal.h"

#define ZSTD_LDM_DEFAULT_WINDOW_LOG ZSTD_WINDOWLOG_LIMIT_DEFAULT

void ZSTD_ldm_fillHashTable(ldmState_t *state, const BYTE *ip, const BYTE *iend,
                            ldmParams_t const *params);

size_t ZSTD_ldm_generateSequences(ldmState_t *ldms, RawSeqStore_t *sequences,
                                  ldmParams_t const *params, void const *src,
                                  size_t srcSize);

size_t ZSTD_ldm_blockCompress(RawSeqStore_t *rawSeqStore, ZSTD_MatchState_t *ms,
                              SeqStore_t *seqStore, U32 rep[ZSTD_REP_NUM],
                              ZSTD_ParamSwitch_e useRowMatchFinder,
                              void const *src, size_t srcSize);

void ZSTD_ldm_skipSequences(RawSeqStore_t *rawSeqStore, size_t srcSize,
                            U32 minMatch);

void ZSTD_ldm_skipRawSeqStoreBytes(RawSeqStore_t *rawSeqStore, size_t nbBytes);

size_t ZSTD_ldm_getTableSize(ldmParams_t params);

size_t ZSTD_ldm_getMaxNbSeq(ldmParams_t params, size_t maxChunkSize);

void ZSTD_ldm_adjustParameters(ldmParams_t *params,
                               ZSTD_compressionParameters const *cParams);

#endif
