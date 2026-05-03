/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "allocations.h"
#include "error_private.h"
#include "hist.h"
#include "mem.h"
#include "zstd_deps.h"
#define FSE_STATIC_LINKING_ONLY
#include "bits.h"
#include "fse.h"
#include "huf.h"
#include "zstd_compress_internal.h"
#include "zstd_compress_literals.h"
#include "zstd_compress_sequences.h"
#include "zstd_compress_superblock.h"
#include "zstd_double_fast.h"
#include "zstd_fast.h"
#include "zstd_lazy.h"
#include "zstd_ldm.h"
#include "zstd_opt.h"

#ifndef ZSTD_COMPRESS_HEAPMODE
#define ZSTD_COMPRESS_HEAPMODE 0
#endif

#ifndef ZSTD_HASHLOG3_MAX
#define ZSTD_HASHLOG3_MAX 17
#endif

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences);

size_t ZSTD_compressBound(size_t srcSize) {
  size_t const r = ZSTD_COMPRESSBOUND(srcSize);
  if (r == 0)
    return ERROR(srcSize_wrong);
  return r;
}

struct ZSTD_CDict_s {
  const void *dictContent;
  size_t dictContentSize;
  ZSTD_dictContentType_e dictContentType;
  U32 *entropyWorkspace;
  ZSTD_cwksp workspace;
  ZSTD_MatchState_t matchState;
  ZSTD_compressedBlockState_t cBlockState;
  ZSTD_customMem customMem;
  U32 dictID;
  int compressionLevel;

  ZSTD_ParamSwitch_e useRowMatchFinder;
};

ZSTD_CCtx *ZSTD_createCCtx(void) {
  return ZSTD_createCCtx_advanced(ZSTD_defaultCMem);
}

static void ZSTD_initCCtx(ZSTD_CCtx *cctx, ZSTD_customMem memManager) {
  assert(cctx != NULL);
  ZSTD_memset(cctx, 0, sizeof(*cctx));
  cctx->customMem = memManager;
  cctx->bmi2 = ZSTD_cpuSupportsBmi2();
  {
    size_t const err = ZSTD_CCtx_reset(cctx, ZSTD_reset_parameters);
    assert(!ZSTD_isError(err));
    (void)err;
  }
}

ZSTD_CCtx *ZSTD_createCCtx_advanced(ZSTD_customMem customMem) {
  ZSTD_STATIC_ASSERT(zcss_init == 0);
  ZSTD_STATIC_ASSERT(ZSTD_CONTENTSIZE_UNKNOWN == (0ULL - 1));
  if ((!customMem.customAlloc) ^ (!customMem.customFree))
    return NULL;
  {
    ZSTD_CCtx *const cctx =
        (ZSTD_CCtx *)ZSTD_customMalloc(sizeof(ZSTD_CCtx), customMem);
    if (!cctx)
      return NULL;
    ZSTD_initCCtx(cctx, customMem);
    return cctx;
  }
}

ZSTD_CCtx *ZSTD_initStaticCCtx(void *workspace, size_t workspaceSize) {
  ZSTD_cwksp ws;
  ZSTD_CCtx *cctx;
  if (workspaceSize <= sizeof(ZSTD_CCtx))
    return NULL;
  if ((size_t)workspace & 7)
    return NULL;
  ZSTD_cwksp_init(&ws, workspace, workspaceSize, ZSTD_cwksp_static_alloc);

  cctx = (ZSTD_CCtx *)ZSTD_cwksp_reserve_object(&ws, sizeof(ZSTD_CCtx));
  if (cctx == NULL)
    return NULL;

  ZSTD_memset(cctx, 0, sizeof(ZSTD_CCtx));
  ZSTD_cwksp_move(&cctx->workspace, &ws);
  cctx->staticSize = workspaceSize;

  if (!ZSTD_cwksp_check_available(&cctx->workspace,
                                  TMP_WORKSPACE_SIZE +
                                      2 * sizeof(ZSTD_compressedBlockState_t)))
    return NULL;
  cctx->blockState.prevCBlock =
      (ZSTD_compressedBlockState_t *)ZSTD_cwksp_reserve_object(
          &cctx->workspace, sizeof(ZSTD_compressedBlockState_t));
  cctx->blockState.nextCBlock =
      (ZSTD_compressedBlockState_t *)ZSTD_cwksp_reserve_object(
          &cctx->workspace, sizeof(ZSTD_compressedBlockState_t));
  cctx->tmpWorkspace =
      ZSTD_cwksp_reserve_object(&cctx->workspace, TMP_WORKSPACE_SIZE);
  cctx->tmpWkspSize = TMP_WORKSPACE_SIZE;
  cctx->bmi2 = ZSTD_cpuid_bmi2(ZSTD_cpuid());
  return cctx;
}

static void ZSTD_clearAllDicts(ZSTD_CCtx *cctx) {
  ZSTD_customFree(cctx->localDict.dictBuffer, cctx->customMem);
  ZSTD_freeCDict(cctx->localDict.cdict);
  ZSTD_memset(&cctx->localDict, 0, sizeof(cctx->localDict));
  ZSTD_memset(&cctx->prefixDict, 0, sizeof(cctx->prefixDict));
  cctx->cdict = NULL;
}

static size_t ZSTD_sizeof_localDict(ZSTD_localDict dict) {
  size_t const bufferSize = dict.dictBuffer != NULL ? dict.dictSize : 0;
  size_t const cdictSize = ZSTD_sizeof_CDict(dict.cdict);
  return bufferSize + cdictSize;
}

static void ZSTD_freeCCtxContent(ZSTD_CCtx *cctx) {
  assert(cctx != NULL);
  assert(cctx->staticSize == 0);
  ZSTD_clearAllDicts(cctx);
#ifdef ZSTD_MULTITHREAD
  ZSTDMT_freeCCtx(cctx->mtctx);
  cctx->mtctx = NULL;
#endif
  ZSTD_cwksp_free(&cctx->workspace, cctx->customMem);
}

size_t ZSTD_freeCCtx(ZSTD_CCtx *cctx) {
  DEBUGLOG(3, "ZSTD_freeCCtx (address: %p)", (void *)cctx);
  if (cctx == NULL)
    return 0;
  RETURN_ERROR_IF(cctx->staticSize, memory_allocation,
                  "not compatible with static CCtx");
  {
    int cctxInWorkspace = ZSTD_cwksp_owns_buffer(&cctx->workspace, cctx);
    ZSTD_freeCCtxContent(cctx);
    if (!cctxInWorkspace)
      ZSTD_customFree(cctx, cctx->customMem);
  }
  return 0;
}

static size_t ZSTD_sizeof_mtctx(const ZSTD_CCtx *cctx) {
#ifdef ZSTD_MULTITHREAD
  return ZSTDMT_sizeof_CCtx(cctx->mtctx);
#else
  (void)cctx;
  return 0;
#endif
}

size_t ZSTD_sizeof_CCtx(const ZSTD_CCtx *cctx) {
  if (cctx == NULL)
    return 0;

  return (cctx->workspace.workspace == cctx ? 0 : sizeof(*cctx)) +
         ZSTD_cwksp_sizeof(&cctx->workspace) +
         ZSTD_sizeof_localDict(cctx->localDict) + ZSTD_sizeof_mtctx(cctx);
}

size_t ZSTD_sizeof_CStream(const ZSTD_CStream *zcs) {
  return ZSTD_sizeof_CCtx(zcs);
}

const SeqStore_t *ZSTD_getSeqStore(const ZSTD_CCtx *ctx) {
  return &(ctx->seqStore);
}

static int ZSTD_rowMatchFinderSupported(const ZSTD_strategy strategy) {
  return (strategy >= ZSTD_greedy && strategy <= ZSTD_lazy2);
}

static int ZSTD_rowMatchFinderUsed(const ZSTD_strategy strategy,
                                   const ZSTD_ParamSwitch_e mode) {
  assert(mode != ZSTD_ps_auto);
  return ZSTD_rowMatchFinderSupported(strategy) && (mode == ZSTD_ps_enable);
}

static ZSTD_ParamSwitch_e ZSTD_resolveRowMatchFinderMode(
    ZSTD_ParamSwitch_e mode, const ZSTD_compressionParameters *const cParams) {
#ifdef ZSTD_LINUX_KERNEL

  const unsigned kWindowLogLowerBound = 17;
#else
  const unsigned kWindowLogLowerBound = 14;
#endif
  if (mode != ZSTD_ps_auto)
    return mode;

  mode = ZSTD_ps_disable;
  if (!ZSTD_rowMatchFinderSupported(cParams->strategy))
    return mode;
  if (cParams->windowLog > kWindowLogLowerBound)
    mode = ZSTD_ps_enable;
  return mode;
}

static ZSTD_ParamSwitch_e
ZSTD_resolveBlockSplitterMode(ZSTD_ParamSwitch_e mode,
                              const ZSTD_compressionParameters *const cParams) {
  if (mode != ZSTD_ps_auto)
    return mode;
  return (cParams->strategy >= ZSTD_btopt && cParams->windowLog >= 17)
             ? ZSTD_ps_enable
             : ZSTD_ps_disable;
}

static int ZSTD_allocateChainTable(const ZSTD_strategy strategy,
                                   const ZSTD_ParamSwitch_e useRowMatchFinder,
                                   const U32 forDDSDict) {
  assert(useRowMatchFinder != ZSTD_ps_auto);

  return forDDSDict || ((strategy != ZSTD_fast) &&
                        !ZSTD_rowMatchFinderUsed(strategy, useRowMatchFinder));
}

static ZSTD_ParamSwitch_e
ZSTD_resolveEnableLdm(ZSTD_ParamSwitch_e mode,
                      const ZSTD_compressionParameters *const cParams) {
  if (mode != ZSTD_ps_auto)
    return mode;
  return (cParams->strategy >= ZSTD_btopt && cParams->windowLog >= 27)
             ? ZSTD_ps_enable
             : ZSTD_ps_disable;
}

static int ZSTD_resolveExternalSequenceValidation(int mode) { return mode; }

static size_t ZSTD_resolveMaxBlockSize(size_t maxBlockSize) {
  if (maxBlockSize == 0) {
    return ZSTD_BLOCKSIZE_MAX;
  } else {
    return maxBlockSize;
  }
}

static ZSTD_ParamSwitch_e
ZSTD_resolveExternalRepcodeSearch(ZSTD_ParamSwitch_e value, int cLevel) {
  if (value != ZSTD_ps_auto)
    return value;
  if (cLevel < 10) {
    return ZSTD_ps_disable;
  } else {
    return ZSTD_ps_enable;
  }
}

static int
ZSTD_CDictIndicesAreTagged(const ZSTD_compressionParameters *const cParams) {
  return cParams->strategy == ZSTD_fast || cParams->strategy == ZSTD_dfast;
}

static ZSTD_CCtx_params
ZSTD_makeCCtxParamsFromCParams(ZSTD_compressionParameters cParams) {
  ZSTD_CCtx_params cctxParams;

  ZSTD_CCtxParams_init(&cctxParams, ZSTD_CLEVEL_DEFAULT);
  cctxParams.cParams = cParams;

  cctxParams.ldmParams.enableLdm =
      ZSTD_resolveEnableLdm(cctxParams.ldmParams.enableLdm, &cParams);
  if (cctxParams.ldmParams.enableLdm == ZSTD_ps_enable) {
    ZSTD_ldm_adjustParameters(&cctxParams.ldmParams, &cParams);
    assert(cctxParams.ldmParams.hashLog >= cctxParams.ldmParams.bucketSizeLog);
    assert(cctxParams.ldmParams.hashRateLog < 32);
  }
  cctxParams.postBlockSplitter =
      ZSTD_resolveBlockSplitterMode(cctxParams.postBlockSplitter, &cParams);
  cctxParams.useRowMatchFinder =
      ZSTD_resolveRowMatchFinderMode(cctxParams.useRowMatchFinder, &cParams);
  cctxParams.validateSequences =
      ZSTD_resolveExternalSequenceValidation(cctxParams.validateSequences);
  cctxParams.maxBlockSize = ZSTD_resolveMaxBlockSize(cctxParams.maxBlockSize);
  cctxParams.searchForExternalRepcodes = ZSTD_resolveExternalRepcodeSearch(
      cctxParams.searchForExternalRepcodes, cctxParams.compressionLevel);
  assert(!ZSTD_checkCParams(cParams));
  return cctxParams;
}

static ZSTD_CCtx_params *
ZSTD_createCCtxParams_advanced(ZSTD_customMem customMem) {
  ZSTD_CCtx_params *params;
  if ((!customMem.customAlloc) ^ (!customMem.customFree))
    return NULL;
  params = (ZSTD_CCtx_params *)ZSTD_customCalloc(sizeof(ZSTD_CCtx_params),
                                                 customMem);
  if (!params) {
    return NULL;
  }
  ZSTD_CCtxParams_init(params, ZSTD_CLEVEL_DEFAULT);
  params->customMem = customMem;
  return params;
}

ZSTD_CCtx_params *ZSTD_createCCtxParams(void) {
  return ZSTD_createCCtxParams_advanced(ZSTD_defaultCMem);
}

size_t ZSTD_freeCCtxParams(ZSTD_CCtx_params *params) {
  if (params == NULL) {
    return 0;
  }
  ZSTD_customFree(params, params->customMem);
  return 0;
}

size_t ZSTD_CCtxParams_reset(ZSTD_CCtx_params *params) {
  return ZSTD_CCtxParams_init(params, ZSTD_CLEVEL_DEFAULT);
}

size_t ZSTD_CCtxParams_init(ZSTD_CCtx_params *cctxParams,
                            int compressionLevel) {
  RETURN_ERROR_IF(!cctxParams, GENERIC, "NULL pointer!");
  ZSTD_memset(cctxParams, 0, sizeof(*cctxParams));
  cctxParams->compressionLevel = compressionLevel;
  cctxParams->fParams.contentSizeFlag = 1;
  return 0;
}

#define ZSTD_NO_CLEVEL 0

static void ZSTD_CCtxParams_init_internal(ZSTD_CCtx_params *cctxParams,
                                          const ZSTD_parameters *params,
                                          int compressionLevel) {
  assert(!ZSTD_checkCParams(params->cParams));
  ZSTD_memset(cctxParams, 0, sizeof(*cctxParams));
  cctxParams->cParams = params->cParams;
  cctxParams->fParams = params->fParams;

  cctxParams->compressionLevel = compressionLevel;
  cctxParams->useRowMatchFinder = ZSTD_resolveRowMatchFinderMode(
      cctxParams->useRowMatchFinder, &params->cParams);
  cctxParams->postBlockSplitter = ZSTD_resolveBlockSplitterMode(
      cctxParams->postBlockSplitter, &params->cParams);
  cctxParams->ldmParams.enableLdm =
      ZSTD_resolveEnableLdm(cctxParams->ldmParams.enableLdm, &params->cParams);
  cctxParams->validateSequences =
      ZSTD_resolveExternalSequenceValidation(cctxParams->validateSequences);
  cctxParams->maxBlockSize = ZSTD_resolveMaxBlockSize(cctxParams->maxBlockSize);
  cctxParams->searchForExternalRepcodes = ZSTD_resolveExternalRepcodeSearch(
      cctxParams->searchForExternalRepcodes, compressionLevel);
  DEBUGLOG(4,
           "ZSTD_CCtxParams_init_internal: useRowMatchFinder=%d, "
           "useBlockSplitter=%d ldm=%d",
           cctxParams->useRowMatchFinder, cctxParams->postBlockSplitter,
           cctxParams->ldmParams.enableLdm);
}

size_t ZSTD_CCtxParams_init_advanced(ZSTD_CCtx_params *cctxParams,
                                     ZSTD_parameters params) {
  RETURN_ERROR_IF(!cctxParams, GENERIC, "NULL pointer!");
  FORWARD_IF_ERROR(ZSTD_checkCParams(params.cParams), "");
  ZSTD_CCtxParams_init_internal(cctxParams, &params, ZSTD_NO_CLEVEL);
  return 0;
}

static void ZSTD_CCtxParams_setZstdParams(ZSTD_CCtx_params *cctxParams,
                                          const ZSTD_parameters *params) {
  assert(!ZSTD_checkCParams(params->cParams));
  cctxParams->cParams = params->cParams;
  cctxParams->fParams = params->fParams;

  cctxParams->compressionLevel = ZSTD_NO_CLEVEL;
}

ZSTD_bounds ZSTD_cParam_getBounds(ZSTD_cParameter param) {
  ZSTD_bounds bounds = {0, 0, 0};

  switch (param) {
  case ZSTD_c_compressionLevel:
    bounds.lowerBound = ZSTD_minCLevel();
    bounds.upperBound = ZSTD_maxCLevel();
    return bounds;

  case ZSTD_c_windowLog:
    bounds.lowerBound = ZSTD_WINDOWLOG_MIN;
    bounds.upperBound = ZSTD_WINDOWLOG_MAX;
    return bounds;

  case ZSTD_c_hashLog:
    bounds.lowerBound = ZSTD_HASHLOG_MIN;
    bounds.upperBound = ZSTD_HASHLOG_MAX;
    return bounds;

  case ZSTD_c_chainLog:
    bounds.lowerBound = ZSTD_CHAINLOG_MIN;
    bounds.upperBound = ZSTD_CHAINLOG_MAX;
    return bounds;

  case ZSTD_c_searchLog:
    bounds.lowerBound = ZSTD_SEARCHLOG_MIN;
    bounds.upperBound = ZSTD_SEARCHLOG_MAX;
    return bounds;

  case ZSTD_c_minMatch:
    bounds.lowerBound = ZSTD_MINMATCH_MIN;
    bounds.upperBound = ZSTD_MINMATCH_MAX;
    return bounds;

  case ZSTD_c_targetLength:
    bounds.lowerBound = ZSTD_TARGETLENGTH_MIN;
    bounds.upperBound = ZSTD_TARGETLENGTH_MAX;
    return bounds;

  case ZSTD_c_strategy:
    bounds.lowerBound = ZSTD_STRATEGY_MIN;
    bounds.upperBound = ZSTD_STRATEGY_MAX;
    return bounds;

  case ZSTD_c_contentSizeFlag:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_checksumFlag:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_dictIDFlag:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_nbWorkers:
    bounds.lowerBound = 0;
#ifdef ZSTD_MULTITHREAD
    bounds.upperBound = ZSTDMT_NBWORKERS_MAX;
#else
    bounds.upperBound = 0;
#endif
    return bounds;

  case ZSTD_c_jobSize:
    bounds.lowerBound = 0;
#ifdef ZSTD_MULTITHREAD
    bounds.upperBound = ZSTDMT_JOBSIZE_MAX;
#else
    bounds.upperBound = 0;
#endif
    return bounds;

  case ZSTD_c_overlapLog:
#ifdef ZSTD_MULTITHREAD
    bounds.lowerBound = ZSTD_OVERLAPLOG_MIN;
    bounds.upperBound = ZSTD_OVERLAPLOG_MAX;
#else
    bounds.lowerBound = 0;
    bounds.upperBound = 0;
#endif
    return bounds;

  case ZSTD_c_enableDedicatedDictSearch:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_enableLongDistanceMatching:
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  case ZSTD_c_ldmHashLog:
    bounds.lowerBound = ZSTD_LDM_HASHLOG_MIN;
    bounds.upperBound = ZSTD_LDM_HASHLOG_MAX;
    return bounds;

  case ZSTD_c_ldmMinMatch:
    bounds.lowerBound = ZSTD_LDM_MINMATCH_MIN;
    bounds.upperBound = ZSTD_LDM_MINMATCH_MAX;
    return bounds;

  case ZSTD_c_ldmBucketSizeLog:
    bounds.lowerBound = ZSTD_LDM_BUCKETSIZELOG_MIN;
    bounds.upperBound = ZSTD_LDM_BUCKETSIZELOG_MAX;
    return bounds;

  case ZSTD_c_ldmHashRateLog:
    bounds.lowerBound = ZSTD_LDM_HASHRATELOG_MIN;
    bounds.upperBound = ZSTD_LDM_HASHRATELOG_MAX;
    return bounds;

  case ZSTD_c_rsyncable:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_forceMaxWindow:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_format:
    ZSTD_STATIC_ASSERT(ZSTD_f_zstd1 < ZSTD_f_zstd1_magicless);
    bounds.lowerBound = ZSTD_f_zstd1;
    bounds.upperBound = ZSTD_f_zstd1_magicless;

    return bounds;

  case ZSTD_c_forceAttachDict:
    ZSTD_STATIC_ASSERT(ZSTD_dictDefaultAttach < ZSTD_dictForceLoad);
    bounds.lowerBound = ZSTD_dictDefaultAttach;
    bounds.upperBound = ZSTD_dictForceLoad;

    return bounds;

  case ZSTD_c_literalCompressionMode:
    ZSTD_STATIC_ASSERT(ZSTD_ps_auto < ZSTD_ps_enable &&
                       ZSTD_ps_enable < ZSTD_ps_disable);
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  case ZSTD_c_targetCBlockSize:
    bounds.lowerBound = ZSTD_TARGETCBLOCKSIZE_MIN;
    bounds.upperBound = ZSTD_TARGETCBLOCKSIZE_MAX;
    return bounds;

  case ZSTD_c_srcSizeHint:
    bounds.lowerBound = ZSTD_SRCSIZEHINT_MIN;
    bounds.upperBound = ZSTD_SRCSIZEHINT_MAX;
    return bounds;

  case ZSTD_c_stableInBuffer:
  case ZSTD_c_stableOutBuffer:
    bounds.lowerBound = (int)ZSTD_bm_buffered;
    bounds.upperBound = (int)ZSTD_bm_stable;
    return bounds;

  case ZSTD_c_blockDelimiters:
    bounds.lowerBound = (int)ZSTD_sf_noBlockDelimiters;
    bounds.upperBound = (int)ZSTD_sf_explicitBlockDelimiters;
    return bounds;

  case ZSTD_c_validateSequences:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_splitAfterSequences:
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  case ZSTD_c_blockSplitterLevel:
    bounds.lowerBound = 0;
    bounds.upperBound = ZSTD_BLOCKSPLITTER_LEVEL_MAX;
    return bounds;

  case ZSTD_c_useRowMatchFinder:
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  case ZSTD_c_deterministicRefPrefix:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_prefetchCDictTables:
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  case ZSTD_c_enableSeqProducerFallback:
    bounds.lowerBound = 0;
    bounds.upperBound = 1;
    return bounds;

  case ZSTD_c_maxBlockSize:
    bounds.lowerBound = ZSTD_BLOCKSIZE_MAX_MIN;
    bounds.upperBound = ZSTD_BLOCKSIZE_MAX;
    return bounds;

  case ZSTD_c_repcodeResolution:
    bounds.lowerBound = (int)ZSTD_ps_auto;
    bounds.upperBound = (int)ZSTD_ps_disable;
    return bounds;

  default:
    bounds.error = ERROR(parameter_unsupported);
    return bounds;
  }
}

static size_t ZSTD_cParam_clampBounds(ZSTD_cParameter cParam, int *value) {
  ZSTD_bounds const bounds = ZSTD_cParam_getBounds(cParam);
  if (ZSTD_isError(bounds.error))
    return bounds.error;
  if (*value < bounds.lowerBound)
    *value = bounds.lowerBound;
  if (*value > bounds.upperBound)
    *value = bounds.upperBound;
  return 0;
}

#define BOUNDCHECK(cParam, val)                                                \
  do {                                                                         \
    RETURN_ERROR_IF(!ZSTD_cParam_withinBounds(cParam, val),                    \
                    parameter_outOfBound, "Param out of bounds");              \
  } while (0)

static int ZSTD_isUpdateAuthorized(ZSTD_cParameter param) {
  switch (param) {
  case ZSTD_c_compressionLevel:
  case ZSTD_c_hashLog:
  case ZSTD_c_chainLog:
  case ZSTD_c_searchLog:
  case ZSTD_c_minMatch:
  case ZSTD_c_targetLength:
  case ZSTD_c_strategy:
  case ZSTD_c_blockSplitterLevel:
    return 1;

  case ZSTD_c_format:
  case ZSTD_c_windowLog:
  case ZSTD_c_contentSizeFlag:
  case ZSTD_c_checksumFlag:
  case ZSTD_c_dictIDFlag:
  case ZSTD_c_forceMaxWindow:
  case ZSTD_c_nbWorkers:
  case ZSTD_c_jobSize:
  case ZSTD_c_overlapLog:
  case ZSTD_c_rsyncable:
  case ZSTD_c_enableDedicatedDictSearch:
  case ZSTD_c_enableLongDistanceMatching:
  case ZSTD_c_ldmHashLog:
  case ZSTD_c_ldmMinMatch:
  case ZSTD_c_ldmBucketSizeLog:
  case ZSTD_c_ldmHashRateLog:
  case ZSTD_c_forceAttachDict:
  case ZSTD_c_literalCompressionMode:
  case ZSTD_c_targetCBlockSize:
  case ZSTD_c_srcSizeHint:
  case ZSTD_c_stableInBuffer:
  case ZSTD_c_stableOutBuffer:
  case ZSTD_c_blockDelimiters:
  case ZSTD_c_validateSequences:
  case ZSTD_c_splitAfterSequences:
  case ZSTD_c_useRowMatchFinder:
  case ZSTD_c_deterministicRefPrefix:
  case ZSTD_c_prefetchCDictTables:
  case ZSTD_c_enableSeqProducerFallback:
  case ZSTD_c_maxBlockSize:
  case ZSTD_c_repcodeResolution:
  default:
    return 0;
  }
}

size_t ZSTD_CCtx_setParameter(ZSTD_CCtx *cctx, ZSTD_cParameter param,
                              int value) {
  DEBUGLOG(4, "ZSTD_CCtx_setParameter (%i, %i)", (int)param, value);
  if (cctx->streamStage != zcss_init) {
    if (ZSTD_isUpdateAuthorized(param)) {
      cctx->cParamsChanged = 1;
    } else {
      RETURN_ERROR(stage_wrong, "can only set params in cctx init stage");
    }
  }

  switch (param) {
  case ZSTD_c_nbWorkers:
    RETURN_ERROR_IF((value != 0) && cctx->staticSize, parameter_unsupported,
                    "MT not compatible with static alloc");
    break;

  case ZSTD_c_compressionLevel:
  case ZSTD_c_windowLog:
  case ZSTD_c_hashLog:
  case ZSTD_c_chainLog:
  case ZSTD_c_searchLog:
  case ZSTD_c_minMatch:
  case ZSTD_c_targetLength:
  case ZSTD_c_strategy:
  case ZSTD_c_ldmHashRateLog:
  case ZSTD_c_format:
  case ZSTD_c_contentSizeFlag:
  case ZSTD_c_checksumFlag:
  case ZSTD_c_dictIDFlag:
  case ZSTD_c_forceMaxWindow:
  case ZSTD_c_forceAttachDict:
  case ZSTD_c_literalCompressionMode:
  case ZSTD_c_jobSize:
  case ZSTD_c_overlapLog:
  case ZSTD_c_rsyncable:
  case ZSTD_c_enableDedicatedDictSearch:
  case ZSTD_c_enableLongDistanceMatching:
  case ZSTD_c_ldmHashLog:
  case ZSTD_c_ldmMinMatch:
  case ZSTD_c_ldmBucketSizeLog:
  case ZSTD_c_targetCBlockSize:
  case ZSTD_c_srcSizeHint:
  case ZSTD_c_stableInBuffer:
  case ZSTD_c_stableOutBuffer:
  case ZSTD_c_blockDelimiters:
  case ZSTD_c_validateSequences:
  case ZSTD_c_splitAfterSequences:
  case ZSTD_c_blockSplitterLevel:
  case ZSTD_c_useRowMatchFinder:
  case ZSTD_c_deterministicRefPrefix:
  case ZSTD_c_prefetchCDictTables:
  case ZSTD_c_enableSeqProducerFallback:
  case ZSTD_c_maxBlockSize:
  case ZSTD_c_repcodeResolution:
    break;

  default:
    RETURN_ERROR(parameter_unsupported, "unknown parameter");
  }
  return ZSTD_CCtxParams_setParameter(&cctx->requestedParams, param, value);
}

size_t ZSTD_CCtxParams_setParameter(ZSTD_CCtx_params *CCtxParams,
                                    ZSTD_cParameter param, int value) {
  DEBUGLOG(4, "ZSTD_CCtxParams_setParameter (%i, %i)", (int)param, value);
  switch (param) {
  case ZSTD_c_format:
    BOUNDCHECK(ZSTD_c_format, value);
    CCtxParams->format = (ZSTD_format_e)value;
    return (size_t)CCtxParams->format;

  case ZSTD_c_compressionLevel: {
    FORWARD_IF_ERROR(ZSTD_cParam_clampBounds(param, &value), "");
    if (value == 0)
      CCtxParams->compressionLevel = ZSTD_CLEVEL_DEFAULT;
    else
      CCtxParams->compressionLevel = value;
    if (CCtxParams->compressionLevel >= 0)
      return (size_t)CCtxParams->compressionLevel;
    return 0;
  }

  case ZSTD_c_windowLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_windowLog, value);
    CCtxParams->cParams.windowLog = (U32)value;
    return CCtxParams->cParams.windowLog;

  case ZSTD_c_hashLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_hashLog, value);
    CCtxParams->cParams.hashLog = (U32)value;
    return CCtxParams->cParams.hashLog;

  case ZSTD_c_chainLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_chainLog, value);
    CCtxParams->cParams.chainLog = (U32)value;
    return CCtxParams->cParams.chainLog;

  case ZSTD_c_searchLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_searchLog, value);
    CCtxParams->cParams.searchLog = (U32)value;
    return (size_t)value;

  case ZSTD_c_minMatch:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_minMatch, value);
    CCtxParams->cParams.minMatch = (U32)value;
    return CCtxParams->cParams.minMatch;

  case ZSTD_c_targetLength:
    BOUNDCHECK(ZSTD_c_targetLength, value);
    CCtxParams->cParams.targetLength = (U32)value;
    return CCtxParams->cParams.targetLength;

  case ZSTD_c_strategy:
    if (value != 0) {
      BOUNDCHECK(ZSTD_c_strategy, value);
      CCtxParams->cParams.strategy = (ZSTD_strategy)value;
    }
    return (size_t)CCtxParams->cParams.strategy;

  case ZSTD_c_contentSizeFlag:

    DEBUGLOG(4, "set content size flag = %u", (value != 0));
    CCtxParams->fParams.contentSizeFlag = value != 0;
    return (size_t)CCtxParams->fParams.contentSizeFlag;

  case ZSTD_c_checksumFlag:

    CCtxParams->fParams.checksumFlag = value != 0;
    return (size_t)CCtxParams->fParams.checksumFlag;

  case ZSTD_c_dictIDFlag:

    DEBUGLOG(4, "set dictIDFlag = %u", (value != 0));
    CCtxParams->fParams.noDictIDFlag = !value;
    return !CCtxParams->fParams.noDictIDFlag;

  case ZSTD_c_forceMaxWindow:
    CCtxParams->forceWindow = (value != 0);
    return (size_t)CCtxParams->forceWindow;

  case ZSTD_c_forceAttachDict: {
    const ZSTD_dictAttachPref_e pref = (ZSTD_dictAttachPref_e)value;
    BOUNDCHECK(ZSTD_c_forceAttachDict, (int)pref);
    CCtxParams->attachDictPref = pref;
    return CCtxParams->attachDictPref;
  }

  case ZSTD_c_literalCompressionMode: {
    const ZSTD_ParamSwitch_e lcm = (ZSTD_ParamSwitch_e)value;
    BOUNDCHECK(ZSTD_c_literalCompressionMode, (int)lcm);
    CCtxParams->literalCompressionMode = lcm;
    return CCtxParams->literalCompressionMode;
  }

  case ZSTD_c_nbWorkers:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR_IF(value != 0, parameter_unsupported,
                    "not compiled with multithreading");
    return 0;
#else
    FORWARD_IF_ERROR(ZSTD_cParam_clampBounds(param, &value), "");
    CCtxParams->nbWorkers = value;
    return (size_t)(CCtxParams->nbWorkers);
#endif

  case ZSTD_c_jobSize:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR_IF(value != 0, parameter_unsupported,
                    "not compiled with multithreading");
    return 0;
#else

    if (value != 0 && value < ZSTDMT_JOBSIZE_MIN)
      value = ZSTDMT_JOBSIZE_MIN;
    FORWARD_IF_ERROR(ZSTD_cParam_clampBounds(param, &value), "");
    assert(value >= 0);
    CCtxParams->jobSize = (size_t)value;
    return CCtxParams->jobSize;
#endif

  case ZSTD_c_overlapLog:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR_IF(value != 0, parameter_unsupported,
                    "not compiled with multithreading");
    return 0;
#else
    FORWARD_IF_ERROR(ZSTD_cParam_clampBounds(ZSTD_c_overlapLog, &value), "");
    CCtxParams->overlapLog = value;
    return (size_t)CCtxParams->overlapLog;
#endif

  case ZSTD_c_rsyncable:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR_IF(value != 0, parameter_unsupported,
                    "not compiled with multithreading");
    return 0;
#else
    FORWARD_IF_ERROR(ZSTD_cParam_clampBounds(ZSTD_c_overlapLog, &value), "");
    CCtxParams->rsyncable = value;
    return (size_t)CCtxParams->rsyncable;
#endif

  case ZSTD_c_enableDedicatedDictSearch:
    CCtxParams->enableDedicatedDictSearch = (value != 0);
    return (size_t)CCtxParams->enableDedicatedDictSearch;

  case ZSTD_c_enableLongDistanceMatching:
    BOUNDCHECK(ZSTD_c_enableLongDistanceMatching, value);
    CCtxParams->ldmParams.enableLdm = (ZSTD_ParamSwitch_e)value;
    return CCtxParams->ldmParams.enableLdm;

  case ZSTD_c_ldmHashLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_ldmHashLog, value);
    CCtxParams->ldmParams.hashLog = (U32)value;
    return CCtxParams->ldmParams.hashLog;

  case ZSTD_c_ldmMinMatch:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_ldmMinMatch, value);
    CCtxParams->ldmParams.minMatchLength = (U32)value;
    return CCtxParams->ldmParams.minMatchLength;

  case ZSTD_c_ldmBucketSizeLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_ldmBucketSizeLog, value);
    CCtxParams->ldmParams.bucketSizeLog = (U32)value;
    return CCtxParams->ldmParams.bucketSizeLog;

  case ZSTD_c_ldmHashRateLog:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_ldmHashRateLog, value);
    CCtxParams->ldmParams.hashRateLog = (U32)value;
    return CCtxParams->ldmParams.hashRateLog;

  case ZSTD_c_targetCBlockSize:
    if (value != 0) {
      value = MAX(value, ZSTD_TARGETCBLOCKSIZE_MIN);
      BOUNDCHECK(ZSTD_c_targetCBlockSize, value);
    }
    CCtxParams->targetCBlockSize = (U32)value;
    return CCtxParams->targetCBlockSize;

  case ZSTD_c_srcSizeHint:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_srcSizeHint, value);
    CCtxParams->srcSizeHint = value;
    return (size_t)CCtxParams->srcSizeHint;

  case ZSTD_c_stableInBuffer:
    BOUNDCHECK(ZSTD_c_stableInBuffer, value);
    CCtxParams->inBufferMode = (ZSTD_bufferMode_e)value;
    return CCtxParams->inBufferMode;

  case ZSTD_c_stableOutBuffer:
    BOUNDCHECK(ZSTD_c_stableOutBuffer, value);
    CCtxParams->outBufferMode = (ZSTD_bufferMode_e)value;
    return CCtxParams->outBufferMode;

  case ZSTD_c_blockDelimiters:
    BOUNDCHECK(ZSTD_c_blockDelimiters, value);
    CCtxParams->blockDelimiters = (ZSTD_SequenceFormat_e)value;
    return CCtxParams->blockDelimiters;

  case ZSTD_c_validateSequences:
    BOUNDCHECK(ZSTD_c_validateSequences, value);
    CCtxParams->validateSequences = value;
    return (size_t)CCtxParams->validateSequences;

  case ZSTD_c_splitAfterSequences:
    BOUNDCHECK(ZSTD_c_splitAfterSequences, value);
    CCtxParams->postBlockSplitter = (ZSTD_ParamSwitch_e)value;
    return CCtxParams->postBlockSplitter;

  case ZSTD_c_blockSplitterLevel:
    BOUNDCHECK(ZSTD_c_blockSplitterLevel, value);
    CCtxParams->preBlockSplitter_level = value;
    return (size_t)CCtxParams->preBlockSplitter_level;

  case ZSTD_c_useRowMatchFinder:
    BOUNDCHECK(ZSTD_c_useRowMatchFinder, value);
    CCtxParams->useRowMatchFinder = (ZSTD_ParamSwitch_e)value;
    return CCtxParams->useRowMatchFinder;

  case ZSTD_c_deterministicRefPrefix:
    BOUNDCHECK(ZSTD_c_deterministicRefPrefix, value);
    CCtxParams->deterministicRefPrefix = !!value;
    return (size_t)CCtxParams->deterministicRefPrefix;

  case ZSTD_c_prefetchCDictTables:
    BOUNDCHECK(ZSTD_c_prefetchCDictTables, value);
    CCtxParams->prefetchCDictTables = (ZSTD_ParamSwitch_e)value;
    return CCtxParams->prefetchCDictTables;

  case ZSTD_c_enableSeqProducerFallback:
    BOUNDCHECK(ZSTD_c_enableSeqProducerFallback, value);
    CCtxParams->enableMatchFinderFallback = value;
    return (size_t)CCtxParams->enableMatchFinderFallback;

  case ZSTD_c_maxBlockSize:
    if (value != 0)
      BOUNDCHECK(ZSTD_c_maxBlockSize, value);
    assert(value >= 0);
    CCtxParams->maxBlockSize = (size_t)value;
    return CCtxParams->maxBlockSize;

  case ZSTD_c_repcodeResolution:
    BOUNDCHECK(ZSTD_c_repcodeResolution, value);
    CCtxParams->searchForExternalRepcodes = (ZSTD_ParamSwitch_e)value;
    return CCtxParams->searchForExternalRepcodes;

  default:
    RETURN_ERROR(parameter_unsupported, "unknown parameter");
  }
}

size_t ZSTD_CCtx_getParameter(ZSTD_CCtx const *cctx, ZSTD_cParameter param,
                              int *value) {
  return ZSTD_CCtxParams_getParameter(&cctx->requestedParams, param, value);
}

size_t ZSTD_CCtxParams_getParameter(ZSTD_CCtx_params const *CCtxParams,
                                    ZSTD_cParameter param, int *value) {
  switch (param) {
  case ZSTD_c_format:
    *value = (int)CCtxParams->format;
    break;
  case ZSTD_c_compressionLevel:
    *value = CCtxParams->compressionLevel;
    break;
  case ZSTD_c_windowLog:
    *value = (int)CCtxParams->cParams.windowLog;
    break;
  case ZSTD_c_hashLog:
    *value = (int)CCtxParams->cParams.hashLog;
    break;
  case ZSTD_c_chainLog:
    *value = (int)CCtxParams->cParams.chainLog;
    break;
  case ZSTD_c_searchLog:
    *value = (int)CCtxParams->cParams.searchLog;
    break;
  case ZSTD_c_minMatch:
    *value = (int)CCtxParams->cParams.minMatch;
    break;
  case ZSTD_c_targetLength:
    *value = (int)CCtxParams->cParams.targetLength;
    break;
  case ZSTD_c_strategy:
    *value = (int)CCtxParams->cParams.strategy;
    break;
  case ZSTD_c_contentSizeFlag:
    *value = CCtxParams->fParams.contentSizeFlag;
    break;
  case ZSTD_c_checksumFlag:
    *value = CCtxParams->fParams.checksumFlag;
    break;
  case ZSTD_c_dictIDFlag:
    *value = !CCtxParams->fParams.noDictIDFlag;
    break;
  case ZSTD_c_forceMaxWindow:
    *value = CCtxParams->forceWindow;
    break;
  case ZSTD_c_forceAttachDict:
    *value = (int)CCtxParams->attachDictPref;
    break;
  case ZSTD_c_literalCompressionMode:
    *value = (int)CCtxParams->literalCompressionMode;
    break;
  case ZSTD_c_nbWorkers:
#ifndef ZSTD_MULTITHREAD
    assert(CCtxParams->nbWorkers == 0);
#endif
    *value = CCtxParams->nbWorkers;
    break;
  case ZSTD_c_jobSize:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
#else
    assert(CCtxParams->jobSize <= INT_MAX);
    *value = (int)CCtxParams->jobSize;
    break;
#endif
  case ZSTD_c_overlapLog:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
#else
    *value = CCtxParams->overlapLog;
    break;
#endif
  case ZSTD_c_rsyncable:
#ifndef ZSTD_MULTITHREAD
    RETURN_ERROR(parameter_unsupported, "not compiled with multithreading");
#else
    *value = CCtxParams->rsyncable;
    break;
#endif
  case ZSTD_c_enableDedicatedDictSearch:
    *value = CCtxParams->enableDedicatedDictSearch;
    break;
  case ZSTD_c_enableLongDistanceMatching:
    *value = (int)CCtxParams->ldmParams.enableLdm;
    break;
  case ZSTD_c_ldmHashLog:
    *value = (int)CCtxParams->ldmParams.hashLog;
    break;
  case ZSTD_c_ldmMinMatch:
    *value = (int)CCtxParams->ldmParams.minMatchLength;
    break;
  case ZSTD_c_ldmBucketSizeLog:
    *value = (int)CCtxParams->ldmParams.bucketSizeLog;
    break;
  case ZSTD_c_ldmHashRateLog:
    *value = (int)CCtxParams->ldmParams.hashRateLog;
    break;
  case ZSTD_c_targetCBlockSize:
    *value = (int)CCtxParams->targetCBlockSize;
    break;
  case ZSTD_c_srcSizeHint:
    *value = (int)CCtxParams->srcSizeHint;
    break;
  case ZSTD_c_stableInBuffer:
    *value = (int)CCtxParams->inBufferMode;
    break;
  case ZSTD_c_stableOutBuffer:
    *value = (int)CCtxParams->outBufferMode;
    break;
  case ZSTD_c_blockDelimiters:
    *value = (int)CCtxParams->blockDelimiters;
    break;
  case ZSTD_c_validateSequences:
    *value = (int)CCtxParams->validateSequences;
    break;
  case ZSTD_c_splitAfterSequences:
    *value = (int)CCtxParams->postBlockSplitter;
    break;
  case ZSTD_c_blockSplitterLevel:
    *value = CCtxParams->preBlockSplitter_level;
    break;
  case ZSTD_c_useRowMatchFinder:
    *value = (int)CCtxParams->useRowMatchFinder;
    break;
  case ZSTD_c_deterministicRefPrefix:
    *value = (int)CCtxParams->deterministicRefPrefix;
    break;
  case ZSTD_c_prefetchCDictTables:
    *value = (int)CCtxParams->prefetchCDictTables;
    break;
  case ZSTD_c_enableSeqProducerFallback:
    *value = CCtxParams->enableMatchFinderFallback;
    break;
  case ZSTD_c_maxBlockSize:
    *value = (int)CCtxParams->maxBlockSize;
    break;
  case ZSTD_c_repcodeResolution:
    *value = (int)CCtxParams->searchForExternalRepcodes;
    break;
  default:
    RETURN_ERROR(parameter_unsupported, "unknown parameter");
  }
  return 0;
}

size_t ZSTD_CCtx_setParametersUsingCCtxParams(ZSTD_CCtx *cctx,
                                              const ZSTD_CCtx_params *params) {
  DEBUGLOG(4, "ZSTD_CCtx_setParametersUsingCCtxParams");
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "The context is in the wrong stage!");
  RETURN_ERROR_IF(cctx->cdict, stage_wrong,
                  "Can't override parameters with cdict attached (some must "
                  "be inherited from the cdict).");

  cctx->requestedParams = *params;
  return 0;
}

size_t ZSTD_CCtx_setCParams(ZSTD_CCtx *cctx,
                            ZSTD_compressionParameters cparams) {
  ZSTD_STATIC_ASSERT(sizeof(cparams) == 7 * 4);
  DEBUGLOG(4, "ZSTD_CCtx_setCParams");

  FORWARD_IF_ERROR(ZSTD_checkCParams(cparams), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, (int)cparams.windowLog),
      "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_chainLog, (int)cparams.chainLog), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_hashLog, (int)cparams.hashLog), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_searchLog, (int)cparams.searchLog),
      "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, (int)cparams.minMatch), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetLength,
                                          (int)cparams.targetLength),
                   "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, (int)cparams.strategy), "");
  return 0;
}

size_t ZSTD_CCtx_setFParams(ZSTD_CCtx *cctx, ZSTD_frameParameters fparams) {
  ZSTD_STATIC_ASSERT(sizeof(fparams) == 3 * 4);
  DEBUGLOG(4, "ZSTD_CCtx_setFParams");
  FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag,
                                          fparams.contentSizeFlag != 0),
                   "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag,
                                          fparams.checksumFlag != 0),
                   "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setParameter(cctx, ZSTD_c_dictIDFlag,
                                          fparams.noDictIDFlag == 0),
                   "");
  return 0;
}

size_t ZSTD_CCtx_setParams(ZSTD_CCtx *cctx, ZSTD_parameters params) {
  DEBUGLOG(4, "ZSTD_CCtx_setParams");

  FORWARD_IF_ERROR(ZSTD_checkCParams(params.cParams), "");

  FORWARD_IF_ERROR(ZSTD_CCtx_setFParams(cctx, params.fParams), "");

  FORWARD_IF_ERROR(ZSTD_CCtx_setCParams(cctx, params.cParams), "");
  return 0;
}

size_t ZSTD_CCtx_setPledgedSrcSize(ZSTD_CCtx *cctx,
                                   unsigned long long pledgedSrcSize) {
  DEBUGLOG(4, "ZSTD_CCtx_setPledgedSrcSize to %llu bytes", pledgedSrcSize);
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "Can't set pledgedSrcSize when not in init stage.");
  cctx->pledgedSrcSizePlusOne = pledgedSrcSize + 1;
  return 0;
}

static ZSTD_compressionParameters
ZSTD_dedicatedDictSearch_getCParams(int compressionLevel, size_t dictSize);
static int
ZSTD_dedicatedDictSearch_isSupported(const ZSTD_compressionParameters *cParams);
static void
ZSTD_dedicatedDictSearch_revertCParams(ZSTD_compressionParameters *cParams);

static size_t ZSTD_initLocalDict(ZSTD_CCtx *cctx) {
  ZSTD_localDict *const dl = &cctx->localDict;
  if (dl->dict == NULL) {

    assert(dl->dictBuffer == NULL);
    assert(dl->cdict == NULL);
    assert(dl->dictSize == 0);
    return 0;
  }
  if (dl->cdict != NULL) {

    assert(cctx->cdict == dl->cdict);
    return 0;
  }
  assert(dl->dictSize > 0);
  assert(cctx->cdict == NULL);
  assert(cctx->prefixDict.dict == NULL);

  dl->cdict = ZSTD_createCDict_advanced2(
      dl->dict, dl->dictSize, ZSTD_dlm_byRef, dl->dictContentType,
      &cctx->requestedParams, cctx->customMem);
  RETURN_ERROR_IF(!dl->cdict, memory_allocation,
                  "ZSTD_createCDict_advanced failed");
  cctx->cdict = dl->cdict;
  return 0;
}

size_t
ZSTD_CCtx_loadDictionary_advanced(ZSTD_CCtx *cctx, const void *dict,
                                  size_t dictSize,
                                  ZSTD_dictLoadMethod_e dictLoadMethod,
                                  ZSTD_dictContentType_e dictContentType) {
  DEBUGLOG(4, "ZSTD_CCtx_loadDictionary_advanced (size: %u)", (U32)dictSize);
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "Can't load a dictionary when cctx is not in init stage.");
  ZSTD_clearAllDicts(cctx);
  if (dict == NULL || dictSize == 0)
    return 0;
  if (dictLoadMethod == ZSTD_dlm_byRef) {
    cctx->localDict.dict = dict;
  } else {

    void *dictBuffer;
    RETURN_ERROR_IF(
        cctx->staticSize, memory_allocation,
        "static CCtx can't allocate for an internal copy of dictionary");
    dictBuffer = ZSTD_customMalloc(dictSize, cctx->customMem);
    RETURN_ERROR_IF(dictBuffer == NULL, memory_allocation,
                    "allocation failed for dictionary content");
    ZSTD_memcpy(dictBuffer, dict, dictSize);
    cctx->localDict.dictBuffer = dictBuffer;
    cctx->localDict.dict = dictBuffer;
  }
  cctx->localDict.dictSize = dictSize;
  cctx->localDict.dictContentType = dictContentType;
  return 0;
}

size_t ZSTD_CCtx_loadDictionary_byReference(ZSTD_CCtx *cctx, const void *dict,
                                            size_t dictSize) {
  return ZSTD_CCtx_loadDictionary_advanced(cctx, dict, dictSize, ZSTD_dlm_byRef,
                                           ZSTD_dct_auto);
}

size_t ZSTD_CCtx_loadDictionary(ZSTD_CCtx *cctx, const void *dict,
                                size_t dictSize) {
  return ZSTD_CCtx_loadDictionary_advanced(cctx, dict, dictSize,
                                           ZSTD_dlm_byCopy, ZSTD_dct_auto);
}

size_t ZSTD_CCtx_refCDict(ZSTD_CCtx *cctx, const ZSTD_CDict *cdict) {
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "Can't ref a dict when ctx not in init stage.");

  ZSTD_clearAllDicts(cctx);
  cctx->cdict = cdict;
  return 0;
}

size_t ZSTD_CCtx_refThreadPool(ZSTD_CCtx *cctx, ZSTD_threadPool *pool) {
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "Can't ref a pool when ctx not in init stage.");
  cctx->pool = pool;
  return 0;
}

size_t ZSTD_CCtx_refPrefix(ZSTD_CCtx *cctx, const void *prefix,
                           size_t prefixSize) {
  return ZSTD_CCtx_refPrefix_advanced(cctx, prefix, prefixSize,
                                      ZSTD_dct_rawContent);
}

size_t ZSTD_CCtx_refPrefix_advanced(ZSTD_CCtx *cctx, const void *prefix,
                                    size_t prefixSize,
                                    ZSTD_dictContentType_e dictContentType) {
  RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                  "Can't ref a prefix when ctx not in init stage.");
  ZSTD_clearAllDicts(cctx);
  if (prefix != NULL && prefixSize > 0) {
    cctx->prefixDict.dict = prefix;
    cctx->prefixDict.dictSize = prefixSize;
    cctx->prefixDict.dictContentType = dictContentType;
  }
  return 0;
}

size_t ZSTD_CCtx_reset(ZSTD_CCtx *cctx, ZSTD_ResetDirective reset) {
  if ((reset == ZSTD_reset_session_only) ||
      (reset == ZSTD_reset_session_and_parameters)) {
    cctx->streamStage = zcss_init;
    cctx->pledgedSrcSizePlusOne = 0;
  }
  if ((reset == ZSTD_reset_parameters) ||
      (reset == ZSTD_reset_session_and_parameters)) {
    RETURN_ERROR_IF(cctx->streamStage != zcss_init, stage_wrong,
                    "Reset parameters is only possible during init stage.");
    ZSTD_clearAllDicts(cctx);
    return ZSTD_CCtxParams_reset(&cctx->requestedParams);
  }
  return 0;
}

size_t ZSTD_checkCParams(ZSTD_compressionParameters cParams) {
  BOUNDCHECK(ZSTD_c_windowLog, (int)cParams.windowLog);
  BOUNDCHECK(ZSTD_c_chainLog, (int)cParams.chainLog);
  BOUNDCHECK(ZSTD_c_hashLog, (int)cParams.hashLog);
  BOUNDCHECK(ZSTD_c_searchLog, (int)cParams.searchLog);
  BOUNDCHECK(ZSTD_c_minMatch, (int)cParams.minMatch);
  BOUNDCHECK(ZSTD_c_targetLength, (int)cParams.targetLength);
  BOUNDCHECK(ZSTD_c_strategy, (int)cParams.strategy);
  return 0;
}

static ZSTD_compressionParameters
ZSTD_clampCParams(ZSTD_compressionParameters cParams) {
#define CLAMP_TYPE(cParam, val, type)                                          \
  do {                                                                         \
    ZSTD_bounds const bounds = ZSTD_cParam_getBounds(cParam);                  \
    if ((int)val < bounds.lowerBound)                                          \
      val = (type)bounds.lowerBound;                                           \
    else if ((int)val > bounds.upperBound)                                     \
      val = (type)bounds.upperBound;                                           \
  } while (0)
#define CLAMP(cParam, val) CLAMP_TYPE(cParam, val, unsigned)
  CLAMP(ZSTD_c_windowLog, cParams.windowLog);
  CLAMP(ZSTD_c_chainLog, cParams.chainLog);
  CLAMP(ZSTD_c_hashLog, cParams.hashLog);
  CLAMP(ZSTD_c_searchLog, cParams.searchLog);
  CLAMP(ZSTD_c_minMatch, cParams.minMatch);
  CLAMP(ZSTD_c_targetLength, cParams.targetLength);
  CLAMP_TYPE(ZSTD_c_strategy, cParams.strategy, ZSTD_strategy);
  return cParams;
}

U32 ZSTD_cycleLog(U32 hashLog, ZSTD_strategy strat) {
  U32 const btScale = ((U32)strat >= (U32)ZSTD_btlazy2);
  return hashLog - btScale;
}

static U32 ZSTD_dictAndWindowLog(U32 windowLog, U64 srcSize, U64 dictSize) {
  const U64 maxWindowSize = 1ULL << ZSTD_WINDOWLOG_MAX;

  if (dictSize == 0) {
    return windowLog;
  }
  assert(windowLog <= ZSTD_WINDOWLOG_MAX);
  assert(srcSize != ZSTD_CONTENTSIZE_UNKNOWN);
  {
    U64 const windowSize = 1ULL << windowLog;
    U64 const dictAndWindowSize = dictSize + windowSize;

    if (windowSize >= dictSize + srcSize) {
      return windowLog;
    } else if (dictAndWindowSize >= maxWindowSize) {
      return ZSTD_WINDOWLOG_MAX;
    } else {
      return ZSTD_highbit32((U32)dictAndWindowSize - 1) + 1;
    }
  }
}

static ZSTD_compressionParameters
ZSTD_adjustCParams_internal(ZSTD_compressionParameters cPar,
                            unsigned long long srcSize, size_t dictSize,
                            ZSTD_CParamMode_e mode,
                            ZSTD_ParamSwitch_e useRowMatchFinder) {
  const U64 minSrcSize = 513;
  const U64 maxWindowResize = 1ULL << (ZSTD_WINDOWLOG_MAX - 1);
  assert(ZSTD_checkCParams(cPar) == 0);

#ifdef ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_btultra2) {
    cPar.strategy = ZSTD_btultra;
  }
  if (cPar.strategy == ZSTD_btultra) {
    cPar.strategy = ZSTD_btopt;
  }
#endif
#ifdef ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_btopt) {
    cPar.strategy = ZSTD_btlazy2;
  }
#endif
#ifdef ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_btlazy2) {
    cPar.strategy = ZSTD_lazy2;
  }
#endif
#ifdef ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_lazy2) {
    cPar.strategy = ZSTD_lazy;
  }
#endif
#ifdef ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_lazy) {
    cPar.strategy = ZSTD_greedy;
  }
#endif
#ifdef ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_greedy) {
    cPar.strategy = ZSTD_dfast;
  }
#endif
#ifdef ZSTD_EXCLUDE_DFAST_BLOCK_COMPRESSOR
  if (cPar.strategy == ZSTD_dfast) {
    cPar.strategy = ZSTD_fast;
    cPar.targetLength = 0;
  }
#endif

  switch (mode) {
  case ZSTD_cpm_unknown:
  case ZSTD_cpm_noAttachDict:

    break;
  case ZSTD_cpm_createCDict:

    if (dictSize && srcSize == ZSTD_CONTENTSIZE_UNKNOWN)
      srcSize = minSrcSize;
    break;
  case ZSTD_cpm_attachDict:

    dictSize = 0;
    break;
  default:
    assert(0);
    break;
  }

  if ((srcSize <= maxWindowResize) && (dictSize <= maxWindowResize)) {
    U32 const tSize = (U32)(srcSize + dictSize);
    static U32 const hashSizeMin = 1 << ZSTD_HASHLOG_MIN;
    U32 const srcLog = (tSize < hashSizeMin) ? ZSTD_HASHLOG_MIN
                                             : ZSTD_highbit32(tSize - 1) + 1;
    if (cPar.windowLog > srcLog)
      cPar.windowLog = srcLog;
  }
  if (srcSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    U32 const dictAndWindowLog =
        ZSTD_dictAndWindowLog(cPar.windowLog, (U64)srcSize, (U64)dictSize);
    U32 const cycleLog = ZSTD_cycleLog(cPar.chainLog, cPar.strategy);
    if (cPar.hashLog > dictAndWindowLog + 1)
      cPar.hashLog = dictAndWindowLog + 1;
    if (cycleLog > dictAndWindowLog)
      cPar.chainLog -= (cycleLog - dictAndWindowLog);
  }

  if (cPar.windowLog < ZSTD_WINDOWLOG_ABSOLUTEMIN)
    cPar.windowLog = ZSTD_WINDOWLOG_ABSOLUTEMIN;

  if (mode == ZSTD_cpm_createCDict && ZSTD_CDictIndicesAreTagged(&cPar)) {
    U32 const maxShortCacheHashLog = 32 - ZSTD_SHORT_CACHE_TAG_BITS;
    if (cPar.hashLog > maxShortCacheHashLog) {
      cPar.hashLog = maxShortCacheHashLog;
    }
    if (cPar.chainLog > maxShortCacheHashLog) {
      cPar.chainLog = maxShortCacheHashLog;
    }
  }

  if (useRowMatchFinder == ZSTD_ps_auto)
    useRowMatchFinder = ZSTD_ps_enable;

  if (ZSTD_rowMatchFinderUsed(cPar.strategy, useRowMatchFinder)) {

    U32 const rowLog = BOUNDED(4, cPar.searchLog, 6);
    U32 const maxRowHashLog = 32 - ZSTD_ROW_HASH_TAG_BITS;
    U32 const maxHashLog = maxRowHashLog + rowLog;
    assert(cPar.hashLog >= rowLog);
    if (cPar.hashLog > maxHashLog) {
      cPar.hashLog = maxHashLog;
    }
  }

  return cPar;
}

ZSTD_compressionParameters ZSTD_adjustCParams(ZSTD_compressionParameters cPar,
                                              unsigned long long srcSize,
                                              size_t dictSize) {
  cPar = ZSTD_clampCParams(cPar);

  if (srcSize == 0)
    srcSize = ZSTD_CONTENTSIZE_UNKNOWN;
  return ZSTD_adjustCParams_internal(cPar, srcSize, dictSize, ZSTD_cpm_unknown,
                                     ZSTD_ps_auto);
}

static ZSTD_compressionParameters
ZSTD_getCParams_internal(int compressionLevel, unsigned long long srcSizeHint,
                         size_t dictSize, ZSTD_CParamMode_e mode);
static ZSTD_parameters ZSTD_getParams_internal(int compressionLevel,
                                               unsigned long long srcSizeHint,
                                               size_t dictSize,
                                               ZSTD_CParamMode_e mode);

static void ZSTD_overrideCParams(ZSTD_compressionParameters *cParams,
                                 const ZSTD_compressionParameters *overrides) {
  if (overrides->windowLog)
    cParams->windowLog = overrides->windowLog;
  if (overrides->hashLog)
    cParams->hashLog = overrides->hashLog;
  if (overrides->chainLog)
    cParams->chainLog = overrides->chainLog;
  if (overrides->searchLog)
    cParams->searchLog = overrides->searchLog;
  if (overrides->minMatch)
    cParams->minMatch = overrides->minMatch;
  if (overrides->targetLength)
    cParams->targetLength = overrides->targetLength;
  if (overrides->strategy)
    cParams->strategy = overrides->strategy;
}

ZSTD_compressionParameters
ZSTD_getCParamsFromCCtxParams(const ZSTD_CCtx_params *CCtxParams,
                              U64 srcSizeHint, size_t dictSize,
                              ZSTD_CParamMode_e mode) {
  ZSTD_compressionParameters cParams;
  if (srcSizeHint == ZSTD_CONTENTSIZE_UNKNOWN && CCtxParams->srcSizeHint > 0) {
    assert(CCtxParams->srcSizeHint >= 0);
    srcSizeHint = (U64)CCtxParams->srcSizeHint;
  }
  cParams = ZSTD_getCParams_internal(CCtxParams->compressionLevel, srcSizeHint,
                                     dictSize, mode);
  if (CCtxParams->ldmParams.enableLdm == ZSTD_ps_enable)
    cParams.windowLog = ZSTD_LDM_DEFAULT_WINDOW_LOG;
  ZSTD_overrideCParams(&cParams, &CCtxParams->cParams);
  assert(!ZSTD_checkCParams(cParams));

  return ZSTD_adjustCParams_internal(cParams, srcSizeHint, dictSize, mode,
                                     CCtxParams->useRowMatchFinder);
}

static size_t
ZSTD_sizeof_matchState(const ZSTD_compressionParameters *const cParams,
                       const ZSTD_ParamSwitch_e useRowMatchFinder,
                       const int enableDedicatedDictSearch, const U32 forCCtx) {

  size_t const chainSize =
      ZSTD_allocateChainTable(cParams->strategy, useRowMatchFinder,
                              enableDedicatedDictSearch && !forCCtx)
          ? ((size_t)1 << cParams->chainLog)
          : 0;
  size_t const hSize = ((size_t)1) << cParams->hashLog;
  U32 const hashLog3 = (forCCtx && cParams->minMatch == 3)
                           ? MIN(ZSTD_HASHLOG3_MAX, cParams->windowLog)
                           : 0;
  size_t const h3Size = hashLog3 ? ((size_t)1) << hashLog3 : 0;

  size_t const tableSpace =
      chainSize * sizeof(U32) + hSize * sizeof(U32) + h3Size * sizeof(U32);
  size_t const optPotentialSpace =
      ZSTD_cwksp_aligned64_alloc_size((MaxML + 1) * sizeof(U32)) +
      ZSTD_cwksp_aligned64_alloc_size((MaxLL + 1) * sizeof(U32)) +
      ZSTD_cwksp_aligned64_alloc_size((MaxOff + 1) * sizeof(U32)) +
      ZSTD_cwksp_aligned64_alloc_size((1 << Litbits) * sizeof(U32)) +
      ZSTD_cwksp_aligned64_alloc_size(ZSTD_OPT_SIZE * sizeof(ZSTD_match_t)) +
      ZSTD_cwksp_aligned64_alloc_size(ZSTD_OPT_SIZE * sizeof(ZSTD_optimal_t));
  size_t const lazyAdditionalSpace =
      ZSTD_rowMatchFinderUsed(cParams->strategy, useRowMatchFinder)
          ? ZSTD_cwksp_aligned64_alloc_size(hSize)
          : 0;
  size_t const optSpace =
      (forCCtx && (cParams->strategy >= ZSTD_btopt)) ? optPotentialSpace : 0;
  size_t const slackSpace = ZSTD_cwksp_slack_space_required();

  ZSTD_STATIC_ASSERT(ZSTD_HASHLOG_MIN >= 4 && ZSTD_WINDOWLOG_MIN >= 4 &&
                     ZSTD_CHAINLOG_MIN >= 4);
  assert(useRowMatchFinder != ZSTD_ps_auto);

  DEBUGLOG(4, "chainSize: %u - hSize: %u - h3Size: %u", (U32)chainSize,
           (U32)hSize, (U32)h3Size);
  return tableSpace + optSpace + slackSpace + lazyAdditionalSpace;
}

static size_t ZSTD_maxNbSeq(size_t blockSize, unsigned minMatch,
                            int useSequenceProducer) {
  U32 const divider = (minMatch == 3 || useSequenceProducer) ? 3 : 4;
  return blockSize / divider;
}

static size_t ZSTD_estimateCCtxSize_usingCCtxParams_internal(
    const ZSTD_compressionParameters *cParams, const ldmParams_t *ldmParams,
    const int isStatic, const ZSTD_ParamSwitch_e useRowMatchFinder,
    const size_t buffInSize, const size_t buffOutSize, const U64 pledgedSrcSize,
    int useSequenceProducer, size_t maxBlockSize) {
  size_t const windowSize =
      (size_t)BOUNDED(1ULL, 1ULL << cParams->windowLog, pledgedSrcSize);
  size_t const blockSize =
      MIN(ZSTD_resolveMaxBlockSize(maxBlockSize), windowSize);
  size_t const maxNbSeq =
      ZSTD_maxNbSeq(blockSize, cParams->minMatch, useSequenceProducer);
  size_t const tokenSpace =
      ZSTD_cwksp_alloc_size(WILDCOPY_OVERLENGTH + blockSize) +
      ZSTD_cwksp_aligned64_alloc_size(maxNbSeq * sizeof(SeqDef)) +
      3 * ZSTD_cwksp_alloc_size(maxNbSeq * sizeof(BYTE));
  size_t const tmpWorkSpace = ZSTD_cwksp_alloc_size(TMP_WORKSPACE_SIZE);
  size_t const blockStateSpace =
      2 * ZSTD_cwksp_alloc_size(sizeof(ZSTD_compressedBlockState_t));
  size_t const matchStateSize =
      ZSTD_sizeof_matchState(cParams, useRowMatchFinder, 0, 1);

  size_t const ldmSpace = ZSTD_ldm_getTableSize(*ldmParams);
  size_t const maxNbLdmSeq = ZSTD_ldm_getMaxNbSeq(*ldmParams, blockSize);
  size_t const ldmSeqSpace =
      ldmParams->enableLdm == ZSTD_ps_enable
          ? ZSTD_cwksp_aligned64_alloc_size(maxNbLdmSeq * sizeof(rawSeq))
          : 0;

  size_t const bufferSpace =
      ZSTD_cwksp_alloc_size(buffInSize) + ZSTD_cwksp_alloc_size(buffOutSize);

  size_t const cctxSpace =
      isStatic ? ZSTD_cwksp_alloc_size(sizeof(ZSTD_CCtx)) : 0;

  size_t const maxNbExternalSeq = ZSTD_sequenceBound(blockSize);
  size_t const externalSeqSpace =
      useSequenceProducer ? ZSTD_cwksp_aligned64_alloc_size(
                                maxNbExternalSeq * sizeof(ZSTD_Sequence))
                          : 0;

  size_t const neededSpace = cctxSpace + tmpWorkSpace + blockStateSpace +
                             ldmSpace + ldmSeqSpace + matchStateSize +
                             tokenSpace + bufferSpace + externalSeqSpace;

  DEBUGLOG(5, "estimate workspace : %u", (U32)neededSpace);
  return neededSpace;
}

size_t ZSTD_estimateCCtxSize_usingCCtxParams(const ZSTD_CCtx_params *params) {
  ZSTD_compressionParameters const cParams = ZSTD_getCParamsFromCCtxParams(
      params, ZSTD_CONTENTSIZE_UNKNOWN, 0, ZSTD_cpm_noAttachDict);
  ldmParams_t ldmParams = params->ldmParams;
  ZSTD_ParamSwitch_e const useRowMatchFinder =
      ZSTD_resolveRowMatchFinderMode(params->useRowMatchFinder, &cParams);

  RETURN_ERROR_IF(
      params->nbWorkers > 0, GENERIC,
      "Estimate CCtx size is supported for single-threaded compression only.");
  if (ldmParams.enableLdm == ZSTD_ps_enable) {
    ZSTD_ldm_adjustParameters(&ldmParams, &cParams);
  }

  return ZSTD_estimateCCtxSize_usingCCtxParams_internal(
      &cParams, &ldmParams, 1, useRowMatchFinder, 0, 0,
      ZSTD_CONTENTSIZE_UNKNOWN, ZSTD_hasExtSeqProd(params),
      params->maxBlockSize);
}

size_t ZSTD_estimateCCtxSize_usingCParams(ZSTD_compressionParameters cParams) {
  ZSTD_CCtx_params initialParams = ZSTD_makeCCtxParamsFromCParams(cParams);
  if (ZSTD_rowMatchFinderSupported(cParams.strategy)) {

    size_t noRowCCtxSize;
    size_t rowCCtxSize;
    initialParams.useRowMatchFinder = ZSTD_ps_disable;
    noRowCCtxSize = ZSTD_estimateCCtxSize_usingCCtxParams(&initialParams);
    initialParams.useRowMatchFinder = ZSTD_ps_enable;
    rowCCtxSize = ZSTD_estimateCCtxSize_usingCCtxParams(&initialParams);
    return MAX(noRowCCtxSize, rowCCtxSize);
  } else {
    return ZSTD_estimateCCtxSize_usingCCtxParams(&initialParams);
  }
}

static size_t ZSTD_estimateCCtxSize_internal(int compressionLevel) {
  int tier = 0;
  size_t largestSize = 0;
  static const unsigned long long srcSizeTiers[4] = {16 KB, 128 KB, 256 KB,
                                                     ZSTD_CONTENTSIZE_UNKNOWN};
  for (; tier < 4; ++tier) {

    ZSTD_compressionParameters const cParams = ZSTD_getCParams_internal(
        compressionLevel, srcSizeTiers[tier], 0, ZSTD_cpm_noAttachDict);
    largestSize = MAX(ZSTD_estimateCCtxSize_usingCParams(cParams), largestSize);
  }
  return largestSize;
}

size_t ZSTD_estimateCCtxSize(int compressionLevel) {
  int level;
  size_t memBudget = 0;
  for (level = MIN(compressionLevel, 1); level <= compressionLevel; level++) {

    size_t const newMB = ZSTD_estimateCCtxSize_internal(level);
    if (newMB > memBudget)
      memBudget = newMB;
  }
  return memBudget;
}

size_t
ZSTD_estimateCStreamSize_usingCCtxParams(const ZSTD_CCtx_params *params) {
  RETURN_ERROR_IF(
      params->nbWorkers > 0, GENERIC,
      "Estimate CCtx size is supported for single-threaded compression only.");
  {
    ZSTD_compressionParameters const cParams = ZSTD_getCParamsFromCCtxParams(
        params, ZSTD_CONTENTSIZE_UNKNOWN, 0, ZSTD_cpm_noAttachDict);
    ldmParams_t ldmParams = params->ldmParams;
    size_t const blockSize = MIN(ZSTD_resolveMaxBlockSize(params->maxBlockSize),
                                 (size_t)1 << cParams.windowLog);
    size_t const inBuffSize = (params->inBufferMode == ZSTD_bm_buffered)
                                  ? ((size_t)1 << cParams.windowLog) + blockSize
                                  : 0;
    size_t const outBuffSize = (params->outBufferMode == ZSTD_bm_buffered)
                                   ? ZSTD_compressBound(blockSize) + 1
                                   : 0;
    ZSTD_ParamSwitch_e const useRowMatchFinder = ZSTD_resolveRowMatchFinderMode(
        params->useRowMatchFinder, &params->cParams);

    if (ldmParams.enableLdm == ZSTD_ps_enable) {
      ZSTD_ldm_adjustParameters(&ldmParams, &cParams);
    }
    return ZSTD_estimateCCtxSize_usingCCtxParams_internal(
        &cParams, &ldmParams, 1, useRowMatchFinder, inBuffSize, outBuffSize,
        ZSTD_CONTENTSIZE_UNKNOWN, ZSTD_hasExtSeqProd(params),
        params->maxBlockSize);
  }
}

size_t
ZSTD_estimateCStreamSize_usingCParams(ZSTD_compressionParameters cParams) {
  ZSTD_CCtx_params initialParams = ZSTD_makeCCtxParamsFromCParams(cParams);
  if (ZSTD_rowMatchFinderSupported(cParams.strategy)) {

    size_t noRowCCtxSize;
    size_t rowCCtxSize;
    initialParams.useRowMatchFinder = ZSTD_ps_disable;
    noRowCCtxSize = ZSTD_estimateCStreamSize_usingCCtxParams(&initialParams);
    initialParams.useRowMatchFinder = ZSTD_ps_enable;
    rowCCtxSize = ZSTD_estimateCStreamSize_usingCCtxParams(&initialParams);
    return MAX(noRowCCtxSize, rowCCtxSize);
  } else {
    return ZSTD_estimateCStreamSize_usingCCtxParams(&initialParams);
  }
}

static size_t ZSTD_estimateCStreamSize_internal(int compressionLevel) {
  ZSTD_compressionParameters const cParams = ZSTD_getCParams_internal(
      compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN, 0, ZSTD_cpm_noAttachDict);
  return ZSTD_estimateCStreamSize_usingCParams(cParams);
}

size_t ZSTD_estimateCStreamSize(int compressionLevel) {
  int level;
  size_t memBudget = 0;
  for (level = MIN(compressionLevel, 1); level <= compressionLevel; level++) {
    size_t const newMB = ZSTD_estimateCStreamSize_internal(level);
    if (newMB > memBudget)
      memBudget = newMB;
  }
  return memBudget;
}

ZSTD_frameProgression ZSTD_getFrameProgression(const ZSTD_CCtx *cctx) {
#ifdef ZSTD_MULTITHREAD
  if (cctx->appliedParams.nbWorkers > 0) {
    return ZSTDMT_getFrameProgression(cctx->mtctx);
  }
#endif
  {
    ZSTD_frameProgression fp;
    size_t const buffered =
        (cctx->inBuff == NULL) ? 0 : cctx->inBuffPos - cctx->inToCompress;
    if (buffered)
      assert(cctx->inBuffPos >= cctx->inToCompress);
    assert(buffered <= ZSTD_BLOCKSIZE_MAX);
    fp.ingested = cctx->consumedSrcSize + buffered;
    fp.consumed = cctx->consumedSrcSize;
    fp.produced = cctx->producedCSize;
    fp.flushed = cctx->producedCSize;

    fp.currentJobID = 0;
    fp.nbActiveWorkers = 0;
    return fp;
  }
}

size_t ZSTD_toFlushNow(ZSTD_CCtx *cctx) {
#ifdef ZSTD_MULTITHREAD
  if (cctx->appliedParams.nbWorkers > 0) {
    return ZSTDMT_toFlushNow(cctx->mtctx);
  }
#endif
  (void)cctx;
  return 0;
}

static void ZSTD_assertEqualCParams(ZSTD_compressionParameters cParams1,
                                    ZSTD_compressionParameters cParams2) {
  (void)cParams1;
  (void)cParams2;
  assert(cParams1.windowLog == cParams2.windowLog);
  assert(cParams1.chainLog == cParams2.chainLog);
  assert(cParams1.hashLog == cParams2.hashLog);
  assert(cParams1.searchLog == cParams2.searchLog);
  assert(cParams1.minMatch == cParams2.minMatch);
  assert(cParams1.targetLength == cParams2.targetLength);
  assert(cParams1.strategy == cParams2.strategy);
}

void ZSTD_reset_compressedBlockState(ZSTD_compressedBlockState_t *bs) {
  int i;
  for (i = 0; i < ZSTD_REP_NUM; ++i)
    bs->rep[i] = repStartValue[i];
  bs->entropy.huf.repeatMode = HUF_repeat_none;
  bs->entropy.fse.offcode_repeatMode = FSE_repeat_none;
  bs->entropy.fse.matchlength_repeatMode = FSE_repeat_none;
  bs->entropy.fse.litlength_repeatMode = FSE_repeat_none;
}

static void ZSTD_invalidateMatchState(ZSTD_MatchState_t *ms) {
  ZSTD_window_clear(&ms->window);

  ms->nextToUpdate = ms->window.dictLimit;
  ms->loadedDictEnd = 0;
  ms->opt.litLengthSum = 0;
  ms->dictMatchState = NULL;
}

typedef enum { ZSTDcrp_makeClean, ZSTDcrp_leaveDirty } ZSTD_compResetPolicy_e;

typedef enum { ZSTDirp_continue, ZSTDirp_reset } ZSTD_indexResetPolicy_e;

typedef enum {
  ZSTD_resetTarget_CDict,
  ZSTD_resetTarget_CCtx
} ZSTD_resetTarget_e;

static U64 ZSTD_bitmix(U64 val, U64 len) {
  val ^= ZSTD_rotateRight_U64(val, 49) ^ ZSTD_rotateRight_U64(val, 24);
  val *= 0x9FB21C651E98DF25ULL;
  val ^= (val >> 35) + len;
  val *= 0x9FB21C651E98DF25ULL;
  return val ^ (val >> 28);
}

static void ZSTD_advanceHashSalt(ZSTD_MatchState_t *ms) {
  ms->hashSalt =
      ZSTD_bitmix(ms->hashSalt, 8) ^ ZSTD_bitmix((U64)ms->hashSaltEntropy, 4);
}

static size_t
ZSTD_reset_matchState(ZSTD_MatchState_t *ms, ZSTD_cwksp *ws,
                      const ZSTD_compressionParameters *cParams,
                      const ZSTD_ParamSwitch_e useRowMatchFinder,
                      const ZSTD_compResetPolicy_e crp,
                      const ZSTD_indexResetPolicy_e forceResetIndex,
                      const ZSTD_resetTarget_e forWho) {

  size_t const chainSize =
      ZSTD_allocateChainTable(cParams->strategy, useRowMatchFinder,
                              ms->dedicatedDictSearch &&
                                  (forWho == ZSTD_resetTarget_CDict))
          ? ((size_t)1 << cParams->chainLog)
          : 0;
  size_t const hSize = ((size_t)1) << cParams->hashLog;
  U32 const hashLog3 =
      ((forWho == ZSTD_resetTarget_CCtx) && cParams->minMatch == 3)
          ? MIN(ZSTD_HASHLOG3_MAX, cParams->windowLog)
          : 0;
  size_t const h3Size = hashLog3 ? ((size_t)1) << hashLog3 : 0;

  DEBUGLOG(4, "reset indices : %u", forceResetIndex == ZSTDirp_reset);
  assert(useRowMatchFinder != ZSTD_ps_auto);
  if (forceResetIndex == ZSTDirp_reset) {
    ZSTD_window_init(&ms->window);
    ZSTD_cwksp_mark_tables_dirty(ws);
  }

  ms->hashLog3 = hashLog3;
  ms->lazySkipping = 0;

  ZSTD_invalidateMatchState(ms);

  assert(!ZSTD_cwksp_reserve_failed(ws));

  ZSTD_cwksp_clear_tables(ws);

  DEBUGLOG(5, "reserving table space");

  ms->hashTable = (U32 *)ZSTD_cwksp_reserve_table(ws, hSize * sizeof(U32));
  ms->chainTable = (U32 *)ZSTD_cwksp_reserve_table(ws, chainSize * sizeof(U32));
  ms->hashTable3 = (U32 *)ZSTD_cwksp_reserve_table(ws, h3Size * sizeof(U32));
  RETURN_ERROR_IF(ZSTD_cwksp_reserve_failed(ws), memory_allocation,
                  "failed a workspace allocation in ZSTD_reset_matchState");

  DEBUGLOG(4, "reset table : %u", crp != ZSTDcrp_leaveDirty);
  if (crp != ZSTDcrp_leaveDirty) {

    ZSTD_cwksp_clean_tables(ws);
  }

  if (ZSTD_rowMatchFinderUsed(cParams->strategy, useRowMatchFinder)) {

    size_t const tagTableSize = hSize;

    if (forWho == ZSTD_resetTarget_CCtx) {
      ms->tagTable =
          (BYTE *)ZSTD_cwksp_reserve_aligned_init_once(ws, tagTableSize);
      ZSTD_advanceHashSalt(ms);
    } else {

      ms->tagTable = (BYTE *)ZSTD_cwksp_reserve_aligned64(ws, tagTableSize);
      ZSTD_memset(ms->tagTable, 0, tagTableSize);
      ms->hashSalt = 0;
    }
    {
      U32 const rowLog = BOUNDED(4, cParams->searchLog, 6);
      assert(cParams->hashLog >= rowLog);
      ms->rowHashLog = cParams->hashLog - rowLog;
    }
  }

  if ((forWho == ZSTD_resetTarget_CCtx) && (cParams->strategy >= ZSTD_btopt)) {
    DEBUGLOG(4, "reserving optimal parser space");
    ms->opt.litFreq = (unsigned *)ZSTD_cwksp_reserve_aligned64(
        ws, (1 << Litbits) * sizeof(unsigned));
    ms->opt.litLengthFreq = (unsigned *)ZSTD_cwksp_reserve_aligned64(
        ws, (MaxLL + 1) * sizeof(unsigned));
    ms->opt.matchLengthFreq = (unsigned *)ZSTD_cwksp_reserve_aligned64(
        ws, (MaxML + 1) * sizeof(unsigned));
    ms->opt.offCodeFreq = (unsigned *)ZSTD_cwksp_reserve_aligned64(
        ws, (MaxOff + 1) * sizeof(unsigned));
    ms->opt.matchTable = (ZSTD_match_t *)ZSTD_cwksp_reserve_aligned64(
        ws, ZSTD_OPT_SIZE * sizeof(ZSTD_match_t));
    ms->opt.priceTable = (ZSTD_optimal_t *)ZSTD_cwksp_reserve_aligned64(
        ws, ZSTD_OPT_SIZE * sizeof(ZSTD_optimal_t));
  }

  ms->cParams = *cParams;

  RETURN_ERROR_IF(ZSTD_cwksp_reserve_failed(ws), memory_allocation,
                  "failed a workspace allocation in ZSTD_reset_matchState");
  return 0;
}

#define ZSTD_INDEXOVERFLOW_MARGIN (16 MB)
static int ZSTD_indexTooCloseToMax(ZSTD_window_t w) {
  return (size_t)(w.nextSrc - w.base) >
         (ZSTD_CURRENT_MAX - ZSTD_INDEXOVERFLOW_MARGIN);
}

static int ZSTD_dictTooBig(size_t const loadedDictSize) {
  return loadedDictSize > ZSTD_CHUNKSIZE_MAX;
}

static size_t ZSTD_resetCCtx_internal(ZSTD_CCtx *zc,
                                      ZSTD_CCtx_params const *params,
                                      U64 const pledgedSrcSize,
                                      size_t const loadedDictSize,
                                      ZSTD_compResetPolicy_e const crp,
                                      ZSTD_buffered_policy_e const zbuff) {
  ZSTD_cwksp *const ws = &zc->workspace;
  DEBUGLOG(4,
           "ZSTD_resetCCtx_internal: pledgedSrcSize=%u, wlog=%u, "
           "useRowMatchFinder=%d useBlockSplitter=%d",
           (U32)pledgedSrcSize, params->cParams.windowLog,
           (int)params->useRowMatchFinder, (int)params->postBlockSplitter);
  assert(!ZSTD_isError(ZSTD_checkCParams(params->cParams)));

  zc->isFirstBlock = 1;

  zc->appliedParams = *params;
  params = &zc->appliedParams;

  assert(params->useRowMatchFinder != ZSTD_ps_auto);
  assert(params->postBlockSplitter != ZSTD_ps_auto);
  assert(params->ldmParams.enableLdm != ZSTD_ps_auto);
  assert(params->maxBlockSize != 0);
  if (params->ldmParams.enableLdm == ZSTD_ps_enable) {

    ZSTD_ldm_adjustParameters(&zc->appliedParams.ldmParams, &params->cParams);
    assert(params->ldmParams.hashLog >= params->ldmParams.bucketSizeLog);
    assert(params->ldmParams.hashRateLog < 32);
  }

  {
    size_t const windowSize = MAX(
        1, (size_t)MIN(((U64)1 << params->cParams.windowLog), pledgedSrcSize));
    size_t const blockSize = MIN(params->maxBlockSize, windowSize);
    size_t const maxNbSeq = ZSTD_maxNbSeq(blockSize, params->cParams.minMatch,
                                          ZSTD_hasExtSeqProd(params));
    size_t const buffOutSize =
        (zbuff == ZSTDb_buffered && params->outBufferMode == ZSTD_bm_buffered)
            ? ZSTD_compressBound(blockSize) + 1
            : 0;
    size_t const buffInSize =
        (zbuff == ZSTDb_buffered && params->inBufferMode == ZSTD_bm_buffered)
            ? windowSize + blockSize
            : 0;
    size_t const maxNbLdmSeq =
        ZSTD_ldm_getMaxNbSeq(params->ldmParams, blockSize);

    int const indexTooClose =
        ZSTD_indexTooCloseToMax(zc->blockState.matchState.window);
    int const dictTooBig = ZSTD_dictTooBig(loadedDictSize);
    ZSTD_indexResetPolicy_e needsIndexReset =
        (indexTooClose || dictTooBig || !zc->initialized) ? ZSTDirp_reset
                                                          : ZSTDirp_continue;

    size_t const neededSpace = ZSTD_estimateCCtxSize_usingCCtxParams_internal(
        &params->cParams, &params->ldmParams, zc->staticSize != 0,
        params->useRowMatchFinder, buffInSize, buffOutSize, pledgedSrcSize,
        ZSTD_hasExtSeqProd(params), params->maxBlockSize);

    FORWARD_IF_ERROR(neededSpace, "cctx size estimate failed!");

    if (!zc->staticSize)
      ZSTD_cwksp_bump_oversized_duration(ws, 0);

    {
      int const workspaceTooSmall = ZSTD_cwksp_sizeof(ws) < neededSpace;
      int const workspaceWasteful = ZSTD_cwksp_check_wasteful(ws, neededSpace);
      int resizeWorkspace = workspaceTooSmall || workspaceWasteful;
      DEBUGLOG(4, "Need %zu B workspace", neededSpace);
      DEBUGLOG(4, "windowSize: %zu - blockSize: %zu", windowSize, blockSize);

      if (resizeWorkspace) {
        DEBUGLOG(4, "Resize workspaceSize from %zuKB to %zuKB",
                 ZSTD_cwksp_sizeof(ws) >> 10, neededSpace >> 10);

        RETURN_ERROR_IF(zc->staticSize, memory_allocation,
                        "static cctx : no resize");

        needsIndexReset = ZSTDirp_reset;

        ZSTD_cwksp_free(ws, zc->customMem);
        FORWARD_IF_ERROR(ZSTD_cwksp_create(ws, neededSpace, zc->customMem), "");

        DEBUGLOG(5, "reserving object space");

        assert(ZSTD_cwksp_check_available(
            ws, 2 * sizeof(ZSTD_compressedBlockState_t)));
        zc->blockState.prevCBlock =
            (ZSTD_compressedBlockState_t *)ZSTD_cwksp_reserve_object(
                ws, sizeof(ZSTD_compressedBlockState_t));
        RETURN_ERROR_IF(zc->blockState.prevCBlock == NULL, memory_allocation,
                        "couldn't allocate prevCBlock");
        zc->blockState.nextCBlock =
            (ZSTD_compressedBlockState_t *)ZSTD_cwksp_reserve_object(
                ws, sizeof(ZSTD_compressedBlockState_t));
        RETURN_ERROR_IF(zc->blockState.nextCBlock == NULL, memory_allocation,
                        "couldn't allocate nextCBlock");
        zc->tmpWorkspace = ZSTD_cwksp_reserve_object(ws, TMP_WORKSPACE_SIZE);
        RETURN_ERROR_IF(zc->tmpWorkspace == NULL, memory_allocation,
                        "couldn't allocate tmpWorkspace");
        zc->tmpWkspSize = TMP_WORKSPACE_SIZE;
      }
    }

    ZSTD_cwksp_clear(ws);

    zc->blockState.matchState.cParams = params->cParams;
    zc->blockState.matchState.prefetchCDictTables =
        params->prefetchCDictTables == ZSTD_ps_enable;
    zc->pledgedSrcSizePlusOne = pledgedSrcSize + 1;
    zc->consumedSrcSize = 0;
    zc->producedCSize = 0;
    if (pledgedSrcSize == ZSTD_CONTENTSIZE_UNKNOWN)
      zc->appliedParams.fParams.contentSizeFlag = 0;
    DEBUGLOG(4, "pledged content size : %u ; flag : %u",
             (unsigned)pledgedSrcSize,
             zc->appliedParams.fParams.contentSizeFlag);
    zc->blockSizeMax = blockSize;

    XXH64_reset(&zc->xxhState, 0);
    zc->stage = ZSTDcs_init;
    zc->dictID = 0;
    zc->dictContentSize = 0;

    ZSTD_reset_compressedBlockState(zc->blockState.prevCBlock);

    FORWARD_IF_ERROR(
        ZSTD_reset_matchState(&zc->blockState.matchState, ws, &params->cParams,
                              params->useRowMatchFinder, crp, needsIndexReset,
                              ZSTD_resetTarget_CCtx),
        "");

    zc->seqStore.sequencesStart =
        (SeqDef *)ZSTD_cwksp_reserve_aligned64(ws, maxNbSeq * sizeof(SeqDef));

    if (params->ldmParams.enableLdm == ZSTD_ps_enable) {

      size_t const ldmHSize = ((size_t)1) << params->ldmParams.hashLog;
      zc->ldmState.hashTable = (ldmEntry_t *)ZSTD_cwksp_reserve_aligned64(
          ws, ldmHSize * sizeof(ldmEntry_t));
      ZSTD_memset(zc->ldmState.hashTable, 0, ldmHSize * sizeof(ldmEntry_t));
      zc->ldmSequences = (rawSeq *)ZSTD_cwksp_reserve_aligned64(
          ws, maxNbLdmSeq * sizeof(rawSeq));
      zc->maxNbLdmSequences = maxNbLdmSeq;

      ZSTD_window_init(&zc->ldmState.window);
      zc->ldmState.loadedDictEnd = 0;
    }

    if (ZSTD_hasExtSeqProd(params)) {
      size_t const maxNbExternalSeq = ZSTD_sequenceBound(blockSize);
      zc->extSeqBufCapacity = maxNbExternalSeq;
      zc->extSeqBuf = (ZSTD_Sequence *)ZSTD_cwksp_reserve_aligned64(
          ws, maxNbExternalSeq * sizeof(ZSTD_Sequence));
    }

    zc->seqStore.litStart =
        ZSTD_cwksp_reserve_buffer(ws, blockSize + WILDCOPY_OVERLENGTH);
    zc->seqStore.maxNbLit = blockSize;

    zc->bufferedPolicy = zbuff;
    zc->inBuffSize = buffInSize;
    zc->inBuff = (char *)ZSTD_cwksp_reserve_buffer(ws, buffInSize);
    zc->outBuffSize = buffOutSize;
    zc->outBuff = (char *)ZSTD_cwksp_reserve_buffer(ws, buffOutSize);

    if (params->ldmParams.enableLdm == ZSTD_ps_enable) {

      size_t const numBuckets = ((size_t)1)
                                << (params->ldmParams.hashLog -
                                    params->ldmParams.bucketSizeLog);
      zc->ldmState.bucketOffsets = ZSTD_cwksp_reserve_buffer(ws, numBuckets);
      ZSTD_memset(zc->ldmState.bucketOffsets, 0, numBuckets);
    }

    ZSTD_referenceExternalSequences(zc, NULL, 0);
    zc->seqStore.maxNbSeq = maxNbSeq;
    zc->seqStore.llCode =
        ZSTD_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));
    zc->seqStore.mlCode =
        ZSTD_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));
    zc->seqStore.ofCode =
        ZSTD_cwksp_reserve_buffer(ws, maxNbSeq * sizeof(BYTE));

    DEBUGLOG(3, "wksp: finished allocating, %zd bytes remain available",
             ZSTD_cwksp_available_space(ws));
    assert(ZSTD_cwksp_estimated_space_within_bounds(ws, neededSpace));

    zc->initialized = 1;

    return 0;
  }
}

void ZSTD_invalidateRepCodes(ZSTD_CCtx *cctx) {
  int i;
  for (i = 0; i < ZSTD_REP_NUM; i++)
    cctx->blockState.prevCBlock->rep[i] = 0;
  assert(!ZSTD_window_hasExtDict(cctx->blockState.matchState.window));
}

static const size_t attachDictSizeCutoffs[ZSTD_STRATEGY_MAX + 1] = {
    8 KB, 8 KB, 16 KB, 32 KB, 32 KB, 32 KB, 32 KB, 32 KB, 8 KB, 8 KB};

static int ZSTD_shouldAttachDict(const ZSTD_CDict *cdict,
                                 const ZSTD_CCtx_params *params,
                                 U64 pledgedSrcSize) {
  size_t cutoff = attachDictSizeCutoffs[cdict->matchState.cParams.strategy];
  int const dedicatedDictSearch = cdict->matchState.dedicatedDictSearch;
  return dedicatedDictSearch ||
         ((pledgedSrcSize <= cutoff ||
           pledgedSrcSize == ZSTD_CONTENTSIZE_UNKNOWN ||
           params->attachDictPref == ZSTD_dictForceAttach) &&
          params->attachDictPref != ZSTD_dictForceCopy && !params->forceWindow);
}

static size_t ZSTD_resetCCtx_byAttachingCDict(ZSTD_CCtx *cctx,
                                              const ZSTD_CDict *cdict,
                                              ZSTD_CCtx_params params,
                                              U64 pledgedSrcSize,
                                              ZSTD_buffered_policy_e zbuff) {
  DEBUGLOG(4, "ZSTD_resetCCtx_byAttachingCDict() pledgedSrcSize=%llu",
           (unsigned long long)pledgedSrcSize);
  {
    ZSTD_compressionParameters adjusted_cdict_cParams =
        cdict->matchState.cParams;
    unsigned const windowLog = params.cParams.windowLog;
    assert(windowLog != 0);

    if (cdict->matchState.dedicatedDictSearch) {
      ZSTD_dedicatedDictSearch_revertCParams(&adjusted_cdict_cParams);
    }

    params.cParams = ZSTD_adjustCParams_internal(
        adjusted_cdict_cParams, pledgedSrcSize, cdict->dictContentSize,
        ZSTD_cpm_attachDict, params.useRowMatchFinder);
    params.cParams.windowLog = windowLog;
    params.useRowMatchFinder = cdict->useRowMatchFinder;
    FORWARD_IF_ERROR(ZSTD_resetCCtx_internal(cctx, &params, pledgedSrcSize, 0,
                                             ZSTDcrp_makeClean, zbuff),
                     "");
    assert(cctx->appliedParams.cParams.strategy ==
           adjusted_cdict_cParams.strategy);
  }

  {
    const U32 cdictEnd =
        (U32)(cdict->matchState.window.nextSrc - cdict->matchState.window.base);
    const U32 cdictLen = cdictEnd - cdict->matchState.window.dictLimit;
    if (cdictLen == 0) {

      DEBUGLOG(4, "skipping attaching empty dictionary");
    } else {
      DEBUGLOG(4, "attaching dictionary into context");
      cctx->blockState.matchState.dictMatchState = &cdict->matchState;

      if (cctx->blockState.matchState.window.dictLimit < cdictEnd) {
        cctx->blockState.matchState.window.nextSrc =
            cctx->blockState.matchState.window.base + cdictEnd;
        ZSTD_window_clear(&cctx->blockState.matchState.window);
      }

      cctx->blockState.matchState.loadedDictEnd =
          cctx->blockState.matchState.window.dictLimit;
    }
  }

  cctx->dictID = cdict->dictID;
  cctx->dictContentSize = cdict->dictContentSize;

  ZSTD_memcpy(cctx->blockState.prevCBlock, &cdict->cBlockState,
              sizeof(cdict->cBlockState));

  return 0;
}

static void
ZSTD_copyCDictTableIntoCCtx(U32 *dst, U32 const *src, size_t tableSize,
                            ZSTD_compressionParameters const *cParams) {
  if (ZSTD_CDictIndicesAreTagged(cParams)) {

    size_t i;
    for (i = 0; i < tableSize; i++) {
      U32 const taggedIndex = src[i];
      U32 const index = taggedIndex >> ZSTD_SHORT_CACHE_TAG_BITS;
      dst[i] = index;
    }
  } else {
    ZSTD_memcpy(dst, src, tableSize * sizeof(U32));
  }
}

static size_t ZSTD_resetCCtx_byCopyingCDict(ZSTD_CCtx *cctx,
                                            const ZSTD_CDict *cdict,
                                            ZSTD_CCtx_params params,
                                            U64 pledgedSrcSize,
                                            ZSTD_buffered_policy_e zbuff) {
  const ZSTD_compressionParameters *cdict_cParams = &cdict->matchState.cParams;

  assert(!cdict->matchState.dedicatedDictSearch);
  DEBUGLOG(4, "ZSTD_resetCCtx_byCopyingCDict() pledgedSrcSize=%llu",
           (unsigned long long)pledgedSrcSize);

  {
    unsigned const windowLog = params.cParams.windowLog;
    assert(windowLog != 0);

    params.cParams = *cdict_cParams;
    params.cParams.windowLog = windowLog;
    params.useRowMatchFinder = cdict->useRowMatchFinder;
    FORWARD_IF_ERROR(ZSTD_resetCCtx_internal(cctx, &params, pledgedSrcSize, 0,
                                             ZSTDcrp_leaveDirty, zbuff),
                     "");
    assert(cctx->appliedParams.cParams.strategy == cdict_cParams->strategy);
    assert(cctx->appliedParams.cParams.hashLog == cdict_cParams->hashLog);
    assert(cctx->appliedParams.cParams.chainLog == cdict_cParams->chainLog);
  }

  ZSTD_cwksp_mark_tables_dirty(&cctx->workspace);
  assert(params.useRowMatchFinder != ZSTD_ps_auto);

  {
    size_t const chainSize =
        ZSTD_allocateChainTable(cdict_cParams->strategy,
                                cdict->useRowMatchFinder, 0)
            ? ((size_t)1 << cdict_cParams->chainLog)
            : 0;
    size_t const hSize = (size_t)1 << cdict_cParams->hashLog;

    ZSTD_copyCDictTableIntoCCtx(cctx->blockState.matchState.hashTable,
                                cdict->matchState.hashTable, hSize,
                                cdict_cParams);

    if (ZSTD_allocateChainTable(cctx->appliedParams.cParams.strategy,
                                cctx->appliedParams.useRowMatchFinder, 0)) {
      ZSTD_copyCDictTableIntoCCtx(cctx->blockState.matchState.chainTable,
                                  cdict->matchState.chainTable, chainSize,
                                  cdict_cParams);
    }

    if (ZSTD_rowMatchFinderUsed(cdict_cParams->strategy,
                                cdict->useRowMatchFinder)) {
      size_t const tagTableSize = hSize;
      ZSTD_memcpy(cctx->blockState.matchState.tagTable,
                  cdict->matchState.tagTable, tagTableSize);
      cctx->blockState.matchState.hashSalt = cdict->matchState.hashSalt;
    }
  }

  assert(cctx->blockState.matchState.hashLog3 <= 31);
  {
    U32 const h3log = cctx->blockState.matchState.hashLog3;
    size_t const h3Size = h3log ? ((size_t)1 << h3log) : 0;
    assert(cdict->matchState.hashLog3 == 0);
    ZSTD_memset(cctx->blockState.matchState.hashTable3, 0,
                h3Size * sizeof(U32));
  }

  ZSTD_cwksp_mark_tables_clean(&cctx->workspace);

  {
    ZSTD_MatchState_t const *srcMatchState = &cdict->matchState;
    ZSTD_MatchState_t *dstMatchState = &cctx->blockState.matchState;
    dstMatchState->window = srcMatchState->window;
    dstMatchState->nextToUpdate = srcMatchState->nextToUpdate;
    dstMatchState->loadedDictEnd = srcMatchState->loadedDictEnd;
  }

  cctx->dictID = cdict->dictID;
  cctx->dictContentSize = cdict->dictContentSize;

  ZSTD_memcpy(cctx->blockState.prevCBlock, &cdict->cBlockState,
              sizeof(cdict->cBlockState));

  return 0;
}

static size_t ZSTD_resetCCtx_usingCDict(ZSTD_CCtx *cctx,
                                        const ZSTD_CDict *cdict,
                                        const ZSTD_CCtx_params *params,
                                        U64 pledgedSrcSize,
                                        ZSTD_buffered_policy_e zbuff) {

  DEBUGLOG(4, "ZSTD_resetCCtx_usingCDict (pledgedSrcSize=%u)",
           (unsigned)pledgedSrcSize);

  if (ZSTD_shouldAttachDict(cdict, params, pledgedSrcSize)) {
    return ZSTD_resetCCtx_byAttachingCDict(cctx, cdict, *params, pledgedSrcSize,
                                           zbuff);
  } else {
    return ZSTD_resetCCtx_byCopyingCDict(cctx, cdict, *params, pledgedSrcSize,
                                         zbuff);
  }
}

static size_t ZSTD_copyCCtx_internal(ZSTD_CCtx *dstCCtx,
                                     const ZSTD_CCtx *srcCCtx,
                                     ZSTD_frameParameters fParams,
                                     U64 pledgedSrcSize,
                                     ZSTD_buffered_policy_e zbuff) {
  RETURN_ERROR_IF(srcCCtx->stage != ZSTDcs_init, stage_wrong,
                  "Can't copy a ctx that's not in init stage.");
  DEBUGLOG(5, "ZSTD_copyCCtx_internal");
  ZSTD_memcpy(&dstCCtx->customMem, &srcCCtx->customMem, sizeof(ZSTD_customMem));
  {
    ZSTD_CCtx_params params = dstCCtx->requestedParams;

    params.cParams = srcCCtx->appliedParams.cParams;
    assert(srcCCtx->appliedParams.useRowMatchFinder != ZSTD_ps_auto);
    assert(srcCCtx->appliedParams.postBlockSplitter != ZSTD_ps_auto);
    assert(srcCCtx->appliedParams.ldmParams.enableLdm != ZSTD_ps_auto);
    params.useRowMatchFinder = srcCCtx->appliedParams.useRowMatchFinder;
    params.postBlockSplitter = srcCCtx->appliedParams.postBlockSplitter;
    params.ldmParams = srcCCtx->appliedParams.ldmParams;
    params.fParams = fParams;
    params.maxBlockSize = srcCCtx->appliedParams.maxBlockSize;
    ZSTD_resetCCtx_internal(dstCCtx, &params, pledgedSrcSize, 0,
                            ZSTDcrp_leaveDirty, zbuff);
    assert(dstCCtx->appliedParams.cParams.windowLog ==
           srcCCtx->appliedParams.cParams.windowLog);
    assert(dstCCtx->appliedParams.cParams.strategy ==
           srcCCtx->appliedParams.cParams.strategy);
    assert(dstCCtx->appliedParams.cParams.hashLog ==
           srcCCtx->appliedParams.cParams.hashLog);
    assert(dstCCtx->appliedParams.cParams.chainLog ==
           srcCCtx->appliedParams.cParams.chainLog);
    assert(dstCCtx->blockState.matchState.hashLog3 ==
           srcCCtx->blockState.matchState.hashLog3);
  }

  ZSTD_cwksp_mark_tables_dirty(&dstCCtx->workspace);

  {
    size_t const chainSize =
        ZSTD_allocateChainTable(srcCCtx->appliedParams.cParams.strategy,
                                srcCCtx->appliedParams.useRowMatchFinder, 0)
            ? ((size_t)1 << srcCCtx->appliedParams.cParams.chainLog)
            : 0;
    size_t const hSize = (size_t)1 << srcCCtx->appliedParams.cParams.hashLog;
    U32 const h3log = srcCCtx->blockState.matchState.hashLog3;
    size_t const h3Size = h3log ? ((size_t)1 << h3log) : 0;

    ZSTD_memcpy(dstCCtx->blockState.matchState.hashTable,
                srcCCtx->blockState.matchState.hashTable, hSize * sizeof(U32));
    ZSTD_memcpy(dstCCtx->blockState.matchState.chainTable,
                srcCCtx->blockState.matchState.chainTable,
                chainSize * sizeof(U32));
    ZSTD_memcpy(dstCCtx->blockState.matchState.hashTable3,
                srcCCtx->blockState.matchState.hashTable3,
                h3Size * sizeof(U32));
  }

  ZSTD_cwksp_mark_tables_clean(&dstCCtx->workspace);

  {
    const ZSTD_MatchState_t *srcMatchState = &srcCCtx->blockState.matchState;
    ZSTD_MatchState_t *dstMatchState = &dstCCtx->blockState.matchState;
    dstMatchState->window = srcMatchState->window;
    dstMatchState->nextToUpdate = srcMatchState->nextToUpdate;
    dstMatchState->loadedDictEnd = srcMatchState->loadedDictEnd;
  }
  dstCCtx->dictID = srcCCtx->dictID;
  dstCCtx->dictContentSize = srcCCtx->dictContentSize;

  ZSTD_memcpy(dstCCtx->blockState.prevCBlock, srcCCtx->blockState.prevCBlock,
              sizeof(*srcCCtx->blockState.prevCBlock));

  return 0;
}

size_t ZSTD_copyCCtx(ZSTD_CCtx *dstCCtx, const ZSTD_CCtx *srcCCtx,
                     unsigned long long pledgedSrcSize) {
  ZSTD_frameParameters fParams = {1, 0, 0};
  ZSTD_buffered_policy_e const zbuff = srcCCtx->bufferedPolicy;
  ZSTD_STATIC_ASSERT((U32)ZSTDb_buffered == 1);
  if (pledgedSrcSize == 0)
    pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;
  fParams.contentSizeFlag = (pledgedSrcSize != ZSTD_CONTENTSIZE_UNKNOWN);

  return ZSTD_copyCCtx_internal(dstCCtx, srcCCtx, fParams, pledgedSrcSize,
                                zbuff);
}

#define ZSTD_ROWSIZE 16

FORCE_INLINE_TEMPLATE void ZSTD_reduceTable_internal(U32 *const table,
                                                     U32 const size,
                                                     U32 const reducerValue,
                                                     int const preserveMark) {
  int const nbRows = (int)size / ZSTD_ROWSIZE;
  int cellNb = 0;
  int rowNb;

  U32 const reducerThreshold = reducerValue + ZSTD_WINDOW_START_INDEX;
  assert((size & (ZSTD_ROWSIZE - 1)) == 0);
  assert(size < (1U << 31));

#if ZSTD_MEMORY_SANITIZER && !defined(ZSTD_MSAN_DONT_POISON_WORKSPACE)

  __msan_unpoison(table, size * sizeof(U32));
#endif

  for (rowNb = 0; rowNb < nbRows; rowNb++) {
    int column;
    for (column = 0; column < ZSTD_ROWSIZE; column++) {
      U32 newVal;
      if (preserveMark && table[cellNb] == ZSTD_DUBT_UNSORTED_MARK) {

        newVal = ZSTD_DUBT_UNSORTED_MARK;
      } else if (table[cellNb] < reducerThreshold) {
        newVal = 0;
      } else {
        newVal = table[cellNb] - reducerValue;
      }
      table[cellNb] = newVal;
      cellNb++;
    }
  }
}

static void ZSTD_reduceTable(U32 *const table, U32 const size,
                             U32 const reducerValue) {
  ZSTD_reduceTable_internal(table, size, reducerValue, 0);
}

static void ZSTD_reduceTable_btlazy2(U32 *const table, U32 const size,
                                     U32 const reducerValue) {
  ZSTD_reduceTable_internal(table, size, reducerValue, 1);
}

static void ZSTD_reduceIndex(ZSTD_MatchState_t *ms,
                             ZSTD_CCtx_params const *params,
                             const U32 reducerValue) {
  {
    U32 const hSize = (U32)1 << params->cParams.hashLog;
    ZSTD_reduceTable(ms->hashTable, hSize, reducerValue);
  }

  if (ZSTD_allocateChainTable(params->cParams.strategy,
                              params->useRowMatchFinder,
                              (U32)ms->dedicatedDictSearch)) {
    U32 const chainSize = (U32)1 << params->cParams.chainLog;
    if (params->cParams.strategy == ZSTD_btlazy2)
      ZSTD_reduceTable_btlazy2(ms->chainTable, chainSize, reducerValue);
    else
      ZSTD_reduceTable(ms->chainTable, chainSize, reducerValue);
  }

  if (ms->hashLog3) {
    U32 const h3Size = (U32)1 << ms->hashLog3;
    ZSTD_reduceTable(ms->hashTable3, h3Size, reducerValue);
  }
}

int ZSTD_seqToCodes(const SeqStore_t *seqStorePtr) {
  const SeqDef *const sequences = seqStorePtr->sequencesStart;
  BYTE *const llCodeTable = seqStorePtr->llCode;
  BYTE *const ofCodeTable = seqStorePtr->ofCode;
  BYTE *const mlCodeTable = seqStorePtr->mlCode;
  U32 const nbSeq = (U32)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
  U32 u;
  int longOffsets = 0;
  assert(nbSeq <= seqStorePtr->maxNbSeq);
  for (u = 0; u < nbSeq; u++) {
    U32 const llv = sequences[u].litLength;
    U32 const ofCode = ZSTD_highbit32(sequences[u].offBase);
    U32 const mlv = sequences[u].mlBase;
    llCodeTable[u] = (BYTE)ZSTD_LLcode(llv);
    ofCodeTable[u] = (BYTE)ofCode;
    mlCodeTable[u] = (BYTE)ZSTD_MLcode(mlv);
    assert(!(MEM_64bits() && ofCode >= STREAM_ACCUMULATOR_MIN));
    if (MEM_32bits() && ofCode >= STREAM_ACCUMULATOR_MIN)
      longOffsets = 1;
  }
  if (seqStorePtr->longLengthType == ZSTD_llt_literalLength)
    llCodeTable[seqStorePtr->longLengthPos] = MaxLL;
  if (seqStorePtr->longLengthType == ZSTD_llt_matchLength)
    mlCodeTable[seqStorePtr->longLengthPos] = MaxML;
  return longOffsets;
}

static int ZSTD_useTargetCBlockSize(const ZSTD_CCtx_params *cctxParams) {
  DEBUGLOG(5, "ZSTD_useTargetCBlockSize (targetCBlockSize=%zu)",
           cctxParams->targetCBlockSize);
  return (cctxParams->targetCBlockSize != 0);
}

static int ZSTD_blockSplitterEnabled(ZSTD_CCtx_params *cctxParams) {
  DEBUGLOG(5, "ZSTD_blockSplitterEnabled (postBlockSplitter=%d)",
           cctxParams->postBlockSplitter);
  assert(cctxParams->postBlockSplitter != ZSTD_ps_auto);
  return (cctxParams->postBlockSplitter == ZSTD_ps_enable);
}

typedef struct {
  U32 LLtype;
  U32 Offtype;
  U32 MLtype;
  size_t size;
  size_t lastCountSize;

  int longOffsets;
} ZSTD_symbolEncodingTypeStats_t;

static ZSTD_symbolEncodingTypeStats_t ZSTD_buildSequencesStatistics(
    const SeqStore_t *seqStorePtr, size_t nbSeq,
    const ZSTD_fseCTables_t *prevEntropy, ZSTD_fseCTables_t *nextEntropy,
    BYTE *dst, const BYTE *const dstEnd, ZSTD_strategy strategy,
    unsigned *countWorkspace, void *entropyWorkspace, size_t entropyWkspSize) {
  BYTE *const ostart = dst;
  const BYTE *const oend = dstEnd;
  BYTE *op = ostart;
  FSE_CTable *CTable_LitLength = nextEntropy->litlengthCTable;
  FSE_CTable *CTable_OffsetBits = nextEntropy->offcodeCTable;
  FSE_CTable *CTable_MatchLength = nextEntropy->matchlengthCTable;
  const BYTE *const ofCodeTable = seqStorePtr->ofCode;
  const BYTE *const llCodeTable = seqStorePtr->llCode;
  const BYTE *const mlCodeTable = seqStorePtr->mlCode;
  ZSTD_symbolEncodingTypeStats_t stats;
  memset(&stats, 0, sizeof(stats));

  stats.lastCountSize = 0;

  stats.longOffsets = ZSTD_seqToCodes(seqStorePtr);
  assert(op <= oend);
  assert(nbSeq != 0);

  {
    unsigned max = MaxLL;
    size_t const mostFrequent =
        HIST_countFast_wksp(countWorkspace, &max, llCodeTable, nbSeq,
                            entropyWorkspace, entropyWkspSize);
    DEBUGLOG(5, "Building LL table");
    nextEntropy->litlength_repeatMode = prevEntropy->litlength_repeatMode;
    stats.LLtype = ZSTD_selectEncodingType(
        &nextEntropy->litlength_repeatMode, countWorkspace, max, mostFrequent,
        nbSeq, LLFSELog, prevEntropy->litlengthCTable, LL_defaultNorm,
        LL_defaultNormLog, ZSTD_defaultAllowed, strategy);
    assert(set_basic < set_compressed && set_rle < set_compressed);
    assert(!(stats.LLtype < set_compressed &&
             nextEntropy->litlength_repeatMode != FSE_repeat_none));
    {
      size_t const countSize = ZSTD_buildCTable(
          op, (size_t)(oend - op), CTable_LitLength, LLFSELog,
          (SymbolEncodingType_e)stats.LLtype, countWorkspace, max, llCodeTable,
          nbSeq, LL_defaultNorm, LL_defaultNormLog, MaxLL,
          prevEntropy->litlengthCTable, sizeof(prevEntropy->litlengthCTable),
          entropyWorkspace, entropyWkspSize);
      if (ZSTD_isError(countSize)) {
        DEBUGLOG(3, "ZSTD_buildCTable for LitLens failed");
        stats.size = countSize;
        return stats;
      }
      if (stats.LLtype == set_compressed)
        stats.lastCountSize = countSize;
      op += countSize;
      assert(op <= oend);
    }
  }

  {
    unsigned max = MaxOff;
    size_t const mostFrequent =
        HIST_countFast_wksp(countWorkspace, &max, ofCodeTable, nbSeq,
                            entropyWorkspace, entropyWkspSize);

    ZSTD_DefaultPolicy_e const defaultPolicy =
        (max <= DefaultMaxOff) ? ZSTD_defaultAllowed : ZSTD_defaultDisallowed;
    DEBUGLOG(5, "Building OF table");
    nextEntropy->offcode_repeatMode = prevEntropy->offcode_repeatMode;
    stats.Offtype = ZSTD_selectEncodingType(
        &nextEntropy->offcode_repeatMode, countWorkspace, max, mostFrequent,
        nbSeq, OffFSELog, prevEntropy->offcodeCTable, OF_defaultNorm,
        OF_defaultNormLog, defaultPolicy, strategy);
    assert(!(stats.Offtype < set_compressed &&
             nextEntropy->offcode_repeatMode != FSE_repeat_none));
    {
      size_t const countSize = ZSTD_buildCTable(
          op, (size_t)(oend - op), CTable_OffsetBits, OffFSELog,
          (SymbolEncodingType_e)stats.Offtype, countWorkspace, max, ofCodeTable,
          nbSeq, OF_defaultNorm, OF_defaultNormLog, DefaultMaxOff,
          prevEntropy->offcodeCTable, sizeof(prevEntropy->offcodeCTable),
          entropyWorkspace, entropyWkspSize);
      if (ZSTD_isError(countSize)) {
        DEBUGLOG(3, "ZSTD_buildCTable for Offsets failed");
        stats.size = countSize;
        return stats;
      }
      if (stats.Offtype == set_compressed)
        stats.lastCountSize = countSize;
      op += countSize;
      assert(op <= oend);
    }
  }

  {
    unsigned max = MaxML;
    size_t const mostFrequent =
        HIST_countFast_wksp(countWorkspace, &max, mlCodeTable, nbSeq,
                            entropyWorkspace, entropyWkspSize);
    DEBUGLOG(5, "Building ML table (remaining space : %i)", (int)(oend - op));
    nextEntropy->matchlength_repeatMode = prevEntropy->matchlength_repeatMode;
    stats.MLtype = ZSTD_selectEncodingType(
        &nextEntropy->matchlength_repeatMode, countWorkspace, max, mostFrequent,
        nbSeq, MLFSELog, prevEntropy->matchlengthCTable, ML_defaultNorm,
        ML_defaultNormLog, ZSTD_defaultAllowed, strategy);
    assert(!(stats.MLtype < set_compressed &&
             nextEntropy->matchlength_repeatMode != FSE_repeat_none));
    {
      size_t const countSize = ZSTD_buildCTable(
          op, (size_t)(oend - op), CTable_MatchLength, MLFSELog,
          (SymbolEncodingType_e)stats.MLtype, countWorkspace, max, mlCodeTable,
          nbSeq, ML_defaultNorm, ML_defaultNormLog, MaxML,
          prevEntropy->matchlengthCTable,
          sizeof(prevEntropy->matchlengthCTable), entropyWorkspace,
          entropyWkspSize);
      if (ZSTD_isError(countSize)) {
        DEBUGLOG(3, "ZSTD_buildCTable for MatchLengths failed");
        stats.size = countSize;
        return stats;
      }
      if (stats.MLtype == set_compressed)
        stats.lastCountSize = countSize;
      op += countSize;
      assert(op <= oend);
    }
  }
  stats.size = (size_t)(op - ostart);
  return stats;
}

#define SUSPECT_UNCOMPRESSIBLE_LITERAL_RATIO 20
MEM_STATIC size_t ZSTD_entropyCompressSeqStore_internal(
    void *dst, size_t dstCapacity, const void *literals, size_t litSize,
    const SeqStore_t *seqStorePtr, const ZSTD_entropyCTables_t *prevEntropy,
    ZSTD_entropyCTables_t *nextEntropy, const ZSTD_CCtx_params *cctxParams,
    void *entropyWorkspace, size_t entropyWkspSize, const int bmi2) {
  ZSTD_strategy const strategy = cctxParams->cParams.strategy;
  unsigned *count = (unsigned *)entropyWorkspace;
  FSE_CTable *CTable_LitLength = nextEntropy->fse.litlengthCTable;
  FSE_CTable *CTable_OffsetBits = nextEntropy->fse.offcodeCTable;
  FSE_CTable *CTable_MatchLength = nextEntropy->fse.matchlengthCTable;
  const SeqDef *const sequences = seqStorePtr->sequencesStart;
  const size_t nbSeq =
      (size_t)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
  const BYTE *const ofCodeTable = seqStorePtr->ofCode;
  const BYTE *const llCodeTable = seqStorePtr->llCode;
  const BYTE *const mlCodeTable = seqStorePtr->mlCode;
  BYTE *const ostart = (BYTE *)dst;
  BYTE *const oend = ostart + dstCapacity;
  BYTE *op = ostart;
  size_t lastCountSize;
  int longOffsets = 0;

  entropyWorkspace = count + (MaxSeq + 1);
  entropyWkspSize -= (MaxSeq + 1) * sizeof(*count);

  DEBUGLOG(5,
           "ZSTD_entropyCompressSeqStore_internal (nbSeq=%zu, dstCapacity=%zu)",
           nbSeq, dstCapacity);
  ZSTD_STATIC_ASSERT(HUF_WORKSPACE_SIZE >= (1 << MAX(MLFSELog, LLFSELog)));
  assert(entropyWkspSize >= HUF_WORKSPACE_SIZE);

  {
    size_t const numSequences =
        (size_t)(seqStorePtr->sequences - seqStorePtr->sequencesStart);

    int const suspectUncompressible =
        (numSequences == 0) ||
        (litSize / numSequences >= SUSPECT_UNCOMPRESSIBLE_LITERAL_RATIO);

    size_t const cSize = ZSTD_compressLiterals(
        op, dstCapacity, literals, litSize, entropyWorkspace, entropyWkspSize,
        &prevEntropy->huf, &nextEntropy->huf, cctxParams->cParams.strategy,
        ZSTD_literalsCompressionIsDisabled(cctxParams), suspectUncompressible,
        bmi2);
    FORWARD_IF_ERROR(cSize, "ZSTD_compressLiterals failed");
    assert(cSize <= dstCapacity);
    op += cSize;
  }

  RETURN_ERROR_IF((oend - op) < 3 + 1, dstSize_tooSmall,
                  "Can't fit seq hdr in output buf!");
  if (nbSeq < 128) {
    *op++ = (BYTE)nbSeq;
  } else if (nbSeq < LONGNBSEQ) {
    op[0] = (BYTE)((nbSeq >> 8) + 0x80);
    op[1] = (BYTE)nbSeq;
    op += 2;
  } else {
    op[0] = 0xFF;
    MEM_writeLE16(op + 1, (U16)(nbSeq - LONGNBSEQ));
    op += 3;
  }
  assert(op <= oend);
  if (nbSeq == 0) {

    ZSTD_memcpy(&nextEntropy->fse, &prevEntropy->fse, sizeof(prevEntropy->fse));
    return (size_t)(op - ostart);
  }
  {
    BYTE *const seqHead = op++;

    const ZSTD_symbolEncodingTypeStats_t stats = ZSTD_buildSequencesStatistics(
        seqStorePtr, nbSeq, &prevEntropy->fse, &nextEntropy->fse, op, oend,
        strategy, count, entropyWorkspace, entropyWkspSize);
    FORWARD_IF_ERROR(stats.size, "ZSTD_buildSequencesStatistics failed!");
    *seqHead = (BYTE)((stats.LLtype << 6) + (stats.Offtype << 4) +
                      (stats.MLtype << 2));
    lastCountSize = stats.lastCountSize;
    op += stats.size;
    longOffsets = stats.longOffsets;
  }

  {
    size_t const bitstreamSize = ZSTD_encodeSequences(
        op, (size_t)(oend - op), CTable_MatchLength, mlCodeTable,
        CTable_OffsetBits, ofCodeTable, CTable_LitLength, llCodeTable,
        sequences, nbSeq, longOffsets, bmi2);
    FORWARD_IF_ERROR(bitstreamSize, "ZSTD_encodeSequences failed");
    op += bitstreamSize;
    assert(op <= oend);

    if (lastCountSize && (lastCountSize + bitstreamSize) < 4) {

      assert(lastCountSize + bitstreamSize == 3);
      DEBUGLOG(5, "Avoiding bug in zstd decoder in versions <= 1.3.4 by "
                  "emitting an uncompressed block.");
      return 0;
    }
  }

  DEBUGLOG(5, "compressed block size : %u", (unsigned)(op - ostart));
  return (size_t)(op - ostart);
}

static size_t ZSTD_entropyCompressSeqStore_wExtLitBuffer(
    void *dst, size_t dstCapacity, const void *literals, size_t litSize,
    size_t blockSize, const SeqStore_t *seqStorePtr,
    const ZSTD_entropyCTables_t *prevEntropy,
    ZSTD_entropyCTables_t *nextEntropy, const ZSTD_CCtx_params *cctxParams,
    void *entropyWorkspace, size_t entropyWkspSize, int bmi2) {
  size_t const cSize = ZSTD_entropyCompressSeqStore_internal(
      dst, dstCapacity, literals, litSize, seqStorePtr, prevEntropy,
      nextEntropy, cctxParams, entropyWorkspace, entropyWkspSize, bmi2);
  if (cSize == 0)
    return 0;

  if ((cSize == ERROR(dstSize_tooSmall)) & (blockSize <= dstCapacity)) {
    DEBUGLOG(4,
             "not enough dstCapacity (%zu) for "
             "ZSTD_entropyCompressSeqStore_internal()=> do not compress block",
             dstCapacity);
    return 0;
  }
  FORWARD_IF_ERROR(cSize, "ZSTD_entropyCompressSeqStore_internal failed");

  {
    size_t const maxCSize =
        blockSize - ZSTD_minGain(blockSize, cctxParams->cParams.strategy);
    if (cSize >= maxCSize)
      return 0;
  }
  DEBUGLOG(5, "ZSTD_entropyCompressSeqStore() cSize: %zu", cSize);

  assert(cSize < ZSTD_BLOCKSIZE_MAX);
  return cSize;
}

static size_t ZSTD_entropyCompressSeqStore(
    const SeqStore_t *seqStorePtr, const ZSTD_entropyCTables_t *prevEntropy,
    ZSTD_entropyCTables_t *nextEntropy, const ZSTD_CCtx_params *cctxParams,
    void *dst, size_t dstCapacity, size_t srcSize, void *entropyWorkspace,
    size_t entropyWkspSize, int bmi2) {
  return ZSTD_entropyCompressSeqStore_wExtLitBuffer(
      dst, dstCapacity, seqStorePtr->litStart,
      (size_t)(seqStorePtr->lit - seqStorePtr->litStart), srcSize, seqStorePtr,
      prevEntropy, nextEntropy, cctxParams, entropyWorkspace, entropyWkspSize,
      bmi2);
}

ZSTD_BlockCompressor_f
ZSTD_selectBlockCompressor(ZSTD_strategy strat,
                           ZSTD_ParamSwitch_e useRowMatchFinder,
                           ZSTD_dictMode_e dictMode) {
  static const ZSTD_BlockCompressor_f blockCompressor[4][ZSTD_STRATEGY_MAX +
                                                         1] = {
      {ZSTD_compressBlock_fast, ZSTD_compressBlock_fast,
       ZSTD_COMPRESSBLOCK_DOUBLEFAST, ZSTD_COMPRESSBLOCK_GREEDY,
       ZSTD_COMPRESSBLOCK_LAZY, ZSTD_COMPRESSBLOCK_LAZY2,
       ZSTD_COMPRESSBLOCK_BTLAZY2, ZSTD_COMPRESSBLOCK_BTOPT,
       ZSTD_COMPRESSBLOCK_BTULTRA, ZSTD_COMPRESSBLOCK_BTULTRA2},
      {ZSTD_compressBlock_fast_extDict, ZSTD_compressBlock_fast_extDict,
       ZSTD_COMPRESSBLOCK_DOUBLEFAST_EXTDICT, ZSTD_COMPRESSBLOCK_GREEDY_EXTDICT,
       ZSTD_COMPRESSBLOCK_LAZY_EXTDICT, ZSTD_COMPRESSBLOCK_LAZY2_EXTDICT,
       ZSTD_COMPRESSBLOCK_BTLAZY2_EXTDICT, ZSTD_COMPRESSBLOCK_BTOPT_EXTDICT,
       ZSTD_COMPRESSBLOCK_BTULTRA_EXTDICT, ZSTD_COMPRESSBLOCK_BTULTRA_EXTDICT},
      {ZSTD_compressBlock_fast_dictMatchState,
       ZSTD_compressBlock_fast_dictMatchState,
       ZSTD_COMPRESSBLOCK_DOUBLEFAST_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_GREEDY_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_LAZY_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_LAZY2_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_BTLAZY2_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_BTOPT_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_BTULTRA_DICTMATCHSTATE,
       ZSTD_COMPRESSBLOCK_BTULTRA_DICTMATCHSTATE},
      {NULL, NULL, NULL, ZSTD_COMPRESSBLOCK_GREEDY_DEDICATEDDICTSEARCH,
       ZSTD_COMPRESSBLOCK_LAZY_DEDICATEDDICTSEARCH,
       ZSTD_COMPRESSBLOCK_LAZY2_DEDICATEDDICTSEARCH, NULL, NULL, NULL, NULL}};
  ZSTD_BlockCompressor_f selectedCompressor;
  ZSTD_STATIC_ASSERT((unsigned)ZSTD_fast == 1);

  assert(ZSTD_cParam_withinBounds(ZSTD_c_strategy, (int)strat));
  DEBUGLOG(5,
           "Selected block compressor: dictMode=%d strat=%d rowMatchfinder=%d",
           (int)dictMode, (int)strat, (int)useRowMatchFinder);
  if (ZSTD_rowMatchFinderUsed(strat, useRowMatchFinder)) {
    static const ZSTD_BlockCompressor_f rowBasedBlockCompressors[4][3] = {
        {ZSTD_COMPRESSBLOCK_GREEDY_ROW, ZSTD_COMPRESSBLOCK_LAZY_ROW,
         ZSTD_COMPRESSBLOCK_LAZY2_ROW},
        {ZSTD_COMPRESSBLOCK_GREEDY_EXTDICT_ROW,
         ZSTD_COMPRESSBLOCK_LAZY_EXTDICT_ROW,
         ZSTD_COMPRESSBLOCK_LAZY2_EXTDICT_ROW},
        {ZSTD_COMPRESSBLOCK_GREEDY_DICTMATCHSTATE_ROW,
         ZSTD_COMPRESSBLOCK_LAZY_DICTMATCHSTATE_ROW,
         ZSTD_COMPRESSBLOCK_LAZY2_DICTMATCHSTATE_ROW},
        {ZSTD_COMPRESSBLOCK_GREEDY_DEDICATEDDICTSEARCH_ROW,
         ZSTD_COMPRESSBLOCK_LAZY_DEDICATEDDICTSEARCH_ROW,
         ZSTD_COMPRESSBLOCK_LAZY2_DEDICATEDDICTSEARCH_ROW}};
    DEBUGLOG(5, "Selecting a row-based matchfinder");
    assert(useRowMatchFinder != ZSTD_ps_auto);
    selectedCompressor =
        rowBasedBlockCompressors[(int)dictMode][(int)strat - (int)ZSTD_greedy];
  } else {
    selectedCompressor = blockCompressor[(int)dictMode][(int)strat];
  }
  assert(selectedCompressor != NULL);
  return selectedCompressor;
}

static void ZSTD_storeLastLiterals(SeqStore_t *seqStorePtr, const BYTE *anchor,
                                   size_t lastLLSize) {
  ZSTD_memcpy(seqStorePtr->lit, anchor, lastLLSize);
  seqStorePtr->lit += lastLLSize;
}

void ZSTD_resetSeqStore(SeqStore_t *ssPtr) {
  ssPtr->lit = ssPtr->litStart;
  ssPtr->sequences = ssPtr->sequencesStart;
  ssPtr->longLengthType = ZSTD_llt_none;
}

static size_t ZSTD_postProcessSequenceProducerResult(ZSTD_Sequence *outSeqs,
                                                     size_t nbExternalSeqs,
                                                     size_t outSeqsCapacity,
                                                     size_t srcSize) {
  RETURN_ERROR_IF(nbExternalSeqs > outSeqsCapacity, sequenceProducer_failed,
                  "External sequence producer returned error code %lu",
                  (unsigned long)nbExternalSeqs);

  RETURN_ERROR_IF(nbExternalSeqs == 0 && srcSize > 0, sequenceProducer_failed,
                  "Got zero sequences from external sequence producer for a "
                  "non-empty src buffer!");

  if (srcSize == 0) {
    ZSTD_memset(&outSeqs[0], 0, sizeof(ZSTD_Sequence));
    return 1;
  }

  {
    ZSTD_Sequence const lastSeq = outSeqs[nbExternalSeqs - 1];

    if (lastSeq.offset == 0 && lastSeq.matchLength == 0) {
      return nbExternalSeqs;
    }

    RETURN_ERROR_IF(nbExternalSeqs == outSeqsCapacity, sequenceProducer_failed,
                    "nbExternalSeqs == outSeqsCapacity but lastSeq is not a "
                    "block delimiter!");

    ZSTD_memset(&outSeqs[nbExternalSeqs], 0, sizeof(ZSTD_Sequence));
    return nbExternalSeqs + 1;
  }
}

static size_t ZSTD_fastSequenceLengthSum(ZSTD_Sequence const *seqBuf,
                                         size_t seqBufSize) {
  size_t matchLenSum, litLenSum, i;
  matchLenSum = 0;
  litLenSum = 0;
  for (i = 0; i < seqBufSize; i++) {
    litLenSum += seqBuf[i].litLength;
    matchLenSum += seqBuf[i].matchLength;
  }
  return litLenSum + matchLenSum;
}

static void ZSTD_validateSeqStore(const SeqStore_t *seqStore,
                                  const ZSTD_compressionParameters *cParams) {
#if DEBUGLEVEL >= 1
  const SeqDef *seq = seqStore->sequencesStart;
  const SeqDef *const seqEnd = seqStore->sequences;
  size_t const matchLenLowerBound = cParams->minMatch == 3 ? 3 : 4;
  for (; seq < seqEnd; ++seq) {
    const ZSTD_SequenceLength seqLength = ZSTD_getSequenceLength(seqStore, seq);
    assert(seqLength.matchLength >= matchLenLowerBound);
    (void)seqLength;
    (void)matchLenLowerBound;
  }
#else
  (void)seqStore;
  (void)cParams;
#endif
}

static size_t ZSTD_transferSequences_wBlockDelim(
    ZSTD_CCtx *cctx, ZSTD_SequencePosition *seqPos, const ZSTD_Sequence *inSeqs,
    size_t inSeqsSize, const void *src, size_t blockSize,
    ZSTD_ParamSwitch_e externalRepSearch);

typedef enum { ZSTDbss_compress, ZSTDbss_noCompress } ZSTD_BuildSeqStore_e;

static size_t ZSTD_buildSeqStore(ZSTD_CCtx *zc, const void *src,
                                 size_t srcSize) {
  ZSTD_MatchState_t *const ms = &zc->blockState.matchState;
  DEBUGLOG(5, "ZSTD_buildSeqStore (srcSize=%zu)", srcSize);
  assert(srcSize <= ZSTD_BLOCKSIZE_MAX);

  ZSTD_assertEqualCParams(zc->appliedParams.cParams, ms->cParams);

  if (srcSize < MIN_CBLOCK_SIZE + ZSTD_blockHeaderSize + 1 + 1) {
    if (zc->appliedParams.cParams.strategy >= ZSTD_btopt) {
      ZSTD_ldm_skipRawSeqStoreBytes(&zc->externSeqStore, srcSize);
    } else {
      ZSTD_ldm_skipSequences(&zc->externSeqStore, srcSize,
                             zc->appliedParams.cParams.minMatch);
    }
    return ZSTDbss_noCompress;
  }
  ZSTD_resetSeqStore(&(zc->seqStore));

  ms->opt.symbolCosts = &zc->blockState.prevCBlock->entropy;

  ms->opt.literalCompressionMode = zc->appliedParams.literalCompressionMode;

  assert(ms->dictMatchState == NULL ||
         ms->loadedDictEnd == ms->window.dictLimit);

  {
    const BYTE *const base = ms->window.base;
    const BYTE *const istart = (const BYTE *)src;
    const U32 curr = (U32)(istart - base);
    if (sizeof(ptrdiff_t) == 8)
      assert(istart - base < (ptrdiff_t)(U32)(-1));
    if (curr > ms->nextToUpdate + 384)
      ms->nextToUpdate = curr - MIN(192, (U32)(curr - ms->nextToUpdate - 384));
  }

  {
    ZSTD_dictMode_e const dictMode = ZSTD_matchState_dictMode(ms);
    size_t lastLLSize;
    {
      int i;
      for (i = 0; i < ZSTD_REP_NUM; ++i)
        zc->blockState.nextCBlock->rep[i] = zc->blockState.prevCBlock->rep[i];
    }
    if (zc->externSeqStore.pos < zc->externSeqStore.size) {
      assert(zc->appliedParams.ldmParams.enableLdm == ZSTD_ps_disable);

      RETURN_ERROR_IF(ZSTD_hasExtSeqProd(&zc->appliedParams),
                      parameter_combination_unsupported,
                      "Long-distance matching with external sequence producer "
                      "enabled is not currently supported.");

      lastLLSize = ZSTD_ldm_blockCompress(
          &zc->externSeqStore, ms, &zc->seqStore,
          zc->blockState.nextCBlock->rep, zc->appliedParams.useRowMatchFinder,
          src, srcSize);
      assert(zc->externSeqStore.pos <= zc->externSeqStore.size);
    } else if (zc->appliedParams.ldmParams.enableLdm == ZSTD_ps_enable) {
      RawSeqStore_t ldmSeqStore = kNullRawSeqStore;

      RETURN_ERROR_IF(ZSTD_hasExtSeqProd(&zc->appliedParams),
                      parameter_combination_unsupported,
                      "Long-distance matching with external sequence producer "
                      "enabled is not currently supported.");

      ldmSeqStore.seq = zc->ldmSequences;
      ldmSeqStore.capacity = zc->maxNbLdmSequences;

      FORWARD_IF_ERROR(ZSTD_ldm_generateSequences(&zc->ldmState, &ldmSeqStore,
                                                  &zc->appliedParams.ldmParams,
                                                  src, srcSize),
                       "");

      lastLLSize = ZSTD_ldm_blockCompress(
          &ldmSeqStore, ms, &zc->seqStore, zc->blockState.nextCBlock->rep,
          zc->appliedParams.useRowMatchFinder, src, srcSize);
      assert(ldmSeqStore.pos == ldmSeqStore.size);
    } else if (ZSTD_hasExtSeqProd(&zc->appliedParams)) {
      assert(zc->extSeqBufCapacity >= ZSTD_sequenceBound(srcSize));
      assert(zc->appliedParams.extSeqProdFunc != NULL);

      {
        U32 const windowSize = (U32)1 << zc->appliedParams.cParams.windowLog;

        size_t const nbExternalSeqs = (zc->appliedParams.extSeqProdFunc)(
            zc->appliedParams.extSeqProdState, zc->extSeqBuf,
            zc->extSeqBufCapacity, src, srcSize, NULL, 0,
            zc->appliedParams.compressionLevel, windowSize);

        size_t const nbPostProcessedSeqs =
            ZSTD_postProcessSequenceProducerResult(
                zc->extSeqBuf, nbExternalSeqs, zc->extSeqBufCapacity, srcSize);

        if (!ZSTD_isError(nbPostProcessedSeqs)) {
          ZSTD_SequencePosition seqPos = {0, 0, 0};
          size_t const seqLenSum =
              ZSTD_fastSequenceLengthSum(zc->extSeqBuf, nbPostProcessedSeqs);
          RETURN_ERROR_IF(seqLenSum > srcSize, externalSequences_invalid,
                          "External sequences imply too large a block!");
          FORWARD_IF_ERROR(ZSTD_transferSequences_wBlockDelim(
                               zc, &seqPos, zc->extSeqBuf, nbPostProcessedSeqs,
                               src, srcSize,
                               zc->appliedParams.searchForExternalRepcodes),
                           "Failed to copy external sequences to seqStore!");
          ms->ldmSeqStore = NULL;
          DEBUGLOG(5,
                   "Copied %lu sequences from external sequence producer to "
                   "internal seqStore.",
                   (unsigned long)nbExternalSeqs);
          return ZSTDbss_compress;
        }

        if (!zc->appliedParams.enableMatchFinderFallback) {
          return nbPostProcessedSeqs;
        }

        {
          ZSTD_BlockCompressor_f const blockCompressor =
              ZSTD_selectBlockCompressor(zc->appliedParams.cParams.strategy,
                                         zc->appliedParams.useRowMatchFinder,
                                         dictMode);
          ms->ldmSeqStore = NULL;
          DEBUGLOG(5,
                   "External sequence producer returned error code %lu. "
                   "Falling back to internal parser.",
                   (unsigned long)nbExternalSeqs);
          lastLLSize = blockCompressor(
              ms, &zc->seqStore, zc->blockState.nextCBlock->rep, src, srcSize);
        }
      }
    } else {
      ZSTD_BlockCompressor_f const blockCompressor = ZSTD_selectBlockCompressor(
          zc->appliedParams.cParams.strategy,
          zc->appliedParams.useRowMatchFinder, dictMode);
      ms->ldmSeqStore = NULL;
      lastLLSize = blockCompressor(
          ms, &zc->seqStore, zc->blockState.nextCBlock->rep, src, srcSize);
    }
    {
      const BYTE *const lastLiterals = (const BYTE *)src + srcSize - lastLLSize;
      ZSTD_storeLastLiterals(&zc->seqStore, lastLiterals, lastLLSize);
    }
  }
  ZSTD_validateSeqStore(&zc->seqStore, &zc->appliedParams.cParams);
  return ZSTDbss_compress;
}

static size_t ZSTD_copyBlockSequences(SeqCollector *seqCollector,
                                      const SeqStore_t *seqStore,
                                      const U32 prevRepcodes[ZSTD_REP_NUM]) {
  const SeqDef *inSeqs = seqStore->sequencesStart;
  const size_t nbInSequences = (size_t)(seqStore->sequences - inSeqs);
  const size_t nbInLiterals = (size_t)(seqStore->lit - seqStore->litStart);

  ZSTD_Sequence *outSeqs =
      seqCollector->seqIndex == 0
          ? seqCollector->seqStart
          : seqCollector->seqStart + seqCollector->seqIndex;
  const size_t nbOutSequences = nbInSequences + 1;
  size_t nbOutLiterals = 0;
  Repcodes_t repcodes;
  size_t i;

  assert(seqCollector->seqIndex <= seqCollector->maxSequences);
  RETURN_ERROR_IF(nbOutSequences > (size_t)(seqCollector->maxSequences -
                                            seqCollector->seqIndex),
                  dstSize_tooSmall, "Not enough space to copy sequences");

  ZSTD_memcpy(&repcodes, prevRepcodes, sizeof(repcodes));
  for (i = 0; i < nbInSequences; ++i) {
    U32 rawOffset;
    outSeqs[i].litLength = inSeqs[i].litLength;
    outSeqs[i].matchLength = inSeqs[i].mlBase + MINMATCH;
    outSeqs[i].rep = 0;

    if (i == seqStore->longLengthPos) {
      if (seqStore->longLengthType == ZSTD_llt_literalLength) {
        outSeqs[i].litLength += 0x10000;
      } else if (seqStore->longLengthType == ZSTD_llt_matchLength) {
        outSeqs[i].matchLength += 0x10000;
      }
    }

    if (OFFBASE_IS_REPCODE(inSeqs[i].offBase)) {
      const U32 repcode = OFFBASE_TO_REPCODE(inSeqs[i].offBase);
      assert(repcode > 0);
      outSeqs[i].rep = repcode;
      if (outSeqs[i].litLength != 0) {
        rawOffset = repcodes.rep[repcode - 1];
      } else {
        if (repcode == 3) {
          assert(repcodes.rep[0] > 1);
          rawOffset = repcodes.rep[0] - 1;
        } else {
          rawOffset = repcodes.rep[repcode];
        }
      }
    } else {
      rawOffset = OFFBASE_TO_OFFSET(inSeqs[i].offBase);
    }
    outSeqs[i].offset = rawOffset;

    ZSTD_updateRep(repcodes.rep, inSeqs[i].offBase, inSeqs[i].litLength == 0);

    nbOutLiterals += outSeqs[i].litLength;
  }

  assert(nbInLiterals >= nbOutLiterals);
  {
    const size_t lastLLSize = nbInLiterals - nbOutLiterals;
    outSeqs[nbInSequences].litLength = (U32)lastLLSize;
    outSeqs[nbInSequences].matchLength = 0;
    outSeqs[nbInSequences].offset = 0;
    assert(nbOutSequences == nbInSequences + 1);
  }
  seqCollector->seqIndex += nbOutSequences;
  assert(seqCollector->seqIndex <= seqCollector->maxSequences);

  return 0;
}

size_t ZSTD_sequenceBound(size_t srcSize) {
  const size_t maxNbSeq = (srcSize / ZSTD_MINMATCH_MIN) + 1;
  const size_t maxNbDelims = (srcSize / ZSTD_BLOCKSIZE_MAX_MIN) + 1;
  return maxNbSeq + maxNbDelims;
}

size_t ZSTD_generateSequences(ZSTD_CCtx *zc, ZSTD_Sequence *outSeqs,
                              size_t outSeqsSize, const void *src,
                              size_t srcSize) {
  const size_t dstCapacity = ZSTD_compressBound(srcSize);
  void *dst;
  SeqCollector seqCollector;
  {
    int targetCBlockSize;
    FORWARD_IF_ERROR(
        ZSTD_CCtx_getParameter(zc, ZSTD_c_targetCBlockSize, &targetCBlockSize),
        "");
    RETURN_ERROR_IF(targetCBlockSize != 0, parameter_unsupported,
                    "targetCBlockSize != 0");
  }
  {
    int nbWorkers;
    FORWARD_IF_ERROR(ZSTD_CCtx_getParameter(zc, ZSTD_c_nbWorkers, &nbWorkers),
                     "");
    RETURN_ERROR_IF(nbWorkers != 0, parameter_unsupported, "nbWorkers != 0");
  }

  dst = ZSTD_customMalloc(dstCapacity, ZSTD_defaultCMem);
  RETURN_ERROR_IF(dst == NULL, memory_allocation, "NULL pointer!");

  seqCollector.collectSequences = 1;
  seqCollector.seqStart = outSeqs;
  seqCollector.seqIndex = 0;
  seqCollector.maxSequences = outSeqsSize;
  zc->seqCollector = seqCollector;

  {
    const size_t ret = ZSTD_compress2(zc, dst, dstCapacity, src, srcSize);
    ZSTD_customFree(dst, ZSTD_defaultCMem);
    FORWARD_IF_ERROR(ret, "ZSTD_compress2 failed");
  }
  assert(zc->seqCollector.seqIndex <= ZSTD_sequenceBound(srcSize));
  return zc->seqCollector.seqIndex;
}

size_t ZSTD_mergeBlockDelimiters(ZSTD_Sequence *sequences, size_t seqsSize) {
  size_t in = 0;
  size_t out = 0;
  for (; in < seqsSize; ++in) {
    if (sequences[in].offset == 0 && sequences[in].matchLength == 0) {
      if (in != seqsSize - 1) {
        sequences[in + 1].litLength += sequences[in].litLength;
      }
    } else {
      sequences[out] = sequences[in];
      ++out;
    }
  }
  return out;
}

static int ZSTD_isRLE(const BYTE *src, size_t length) {
  const BYTE *ip = src;
  const BYTE value = ip[0];
  const size_t valueST = (size_t)((U64)value * 0x0101010101010101ULL);
  const size_t unrollSize = sizeof(size_t) * 4;
  const size_t unrollMask = unrollSize - 1;
  const size_t prefixLength = length & unrollMask;
  size_t i;
  if (length == 1)
    return 1;

  if (prefixLength &&
      ZSTD_count(ip + 1, ip, ip + prefixLength) != prefixLength - 1) {
    return 0;
  }
  for (i = prefixLength; i != length; i += unrollSize) {
    size_t u;
    for (u = 0; u < unrollSize; u += sizeof(size_t)) {
      if (MEM_readST(ip + i + u) != valueST) {
        return 0;
      }
    }
  }
  return 1;
}

static int ZSTD_maybeRLE(SeqStore_t const *seqStore) {
  size_t const nbSeqs =
      (size_t)(seqStore->sequences - seqStore->sequencesStart);
  size_t const nbLits = (size_t)(seqStore->lit - seqStore->litStart);

  return nbSeqs < 4 && nbLits < 10;
}

static void
ZSTD_blockState_confirmRepcodesAndEntropyTables(ZSTD_blockState_t *const bs) {
  ZSTD_compressedBlockState_t *const tmp = bs->prevCBlock;
  bs->prevCBlock = bs->nextCBlock;
  bs->nextCBlock = tmp;
}

static void writeBlockHeader(void *op, size_t cSize, size_t blockSize,
                             U32 lastBlock) {
  U32 const cBlockHeader =
      cSize == 1 ? lastBlock + (((U32)bt_rle) << 1) + (U32)(blockSize << 3)
                 : lastBlock + (((U32)bt_compressed) << 1) + (U32)(cSize << 3);
  MEM_writeLE24(op, cBlockHeader);
  DEBUGLOG(5, "writeBlockHeader: cSize: %zu blockSize: %zu lastBlock: %u",
           cSize, blockSize, lastBlock);
}

static size_t ZSTD_buildBlockEntropyStats_literals(
    void *const src, size_t srcSize, const ZSTD_hufCTables_t *prevHuf,
    ZSTD_hufCTables_t *nextHuf, ZSTD_hufCTablesMetadata_t *hufMetadata,
    const int literalsCompressionIsDisabled, void *workspace, size_t wkspSize,
    int hufFlags) {
  BYTE *const wkspStart = (BYTE *)workspace;
  BYTE *const wkspEnd = wkspStart + wkspSize;
  BYTE *const countWkspStart = wkspStart;
  unsigned *const countWksp = (unsigned *)workspace;
  const size_t countWkspSize = (HUF_SYMBOLVALUE_MAX + 1) * sizeof(unsigned);
  BYTE *const nodeWksp = countWkspStart + countWkspSize;
  const size_t nodeWkspSize = (size_t)(wkspEnd - nodeWksp);
  unsigned maxSymbolValue = HUF_SYMBOLVALUE_MAX;
  unsigned huffLog = LitHufLog;
  HUF_repeat repeat = prevHuf->repeatMode;
  DEBUGLOG(5, "ZSTD_buildBlockEntropyStats_literals (srcSize=%zu)", srcSize);

  ZSTD_memcpy(nextHuf, prevHuf, sizeof(*prevHuf));

  if (literalsCompressionIsDisabled) {
    DEBUGLOG(5, "set_basic - disabled");
    hufMetadata->hType = set_basic;
    return 0;
  }

#ifndef COMPRESS_LITERALS_SIZE_MIN
#define COMPRESS_LITERALS_SIZE_MIN 63
#endif
  {
    size_t const minLitSize = (prevHuf->repeatMode == HUF_repeat_valid)
                                  ? 6
                                  : COMPRESS_LITERALS_SIZE_MIN;
    if (srcSize <= minLitSize) {
      DEBUGLOG(5, "set_basic - too small");
      hufMetadata->hType = set_basic;
      return 0;
    }
  }

  {
    size_t const largest =
        HIST_count_wksp(countWksp, &maxSymbolValue, (const BYTE *)src, srcSize,
                        workspace, wkspSize);
    FORWARD_IF_ERROR(largest, "HIST_count_wksp failed");
    if (largest == srcSize) {

      DEBUGLOG(5, "set_rle");
      hufMetadata->hType = set_rle;
      return 0;
    }
    if (largest <= (srcSize >> 7) + 4) {

      DEBUGLOG(5, "set_basic - no gain");
      hufMetadata->hType = set_basic;
      return 0;
    }
  }

  if (repeat == HUF_repeat_check &&
      !HUF_validateCTable((HUF_CElt const *)prevHuf->CTable, countWksp,
                          maxSymbolValue)) {
    repeat = HUF_repeat_none;
  }

  ZSTD_memset(nextHuf->CTable, 0, sizeof(nextHuf->CTable));
  huffLog =
      HUF_optimalTableLog(huffLog, srcSize, maxSymbolValue, nodeWksp,
                          nodeWkspSize, nextHuf->CTable, countWksp, hufFlags);
  assert(huffLog <= LitHufLog);
  {
    size_t const maxBits =
        HUF_buildCTable_wksp((HUF_CElt *)nextHuf->CTable, countWksp,
                             maxSymbolValue, huffLog, nodeWksp, nodeWkspSize);
    FORWARD_IF_ERROR(maxBits, "HUF_buildCTable_wksp");
    huffLog = (U32)maxBits;
  }
  {
    size_t const newCSize = HUF_estimateCompressedSize(
        (HUF_CElt *)nextHuf->CTable, countWksp, maxSymbolValue);
    size_t const hSize = HUF_writeCTable_wksp(
        hufMetadata->hufDesBuffer, sizeof(hufMetadata->hufDesBuffer),
        (HUF_CElt *)nextHuf->CTable, maxSymbolValue, huffLog, nodeWksp,
        nodeWkspSize);

    if (repeat != HUF_repeat_none) {
      size_t const oldCSize = HUF_estimateCompressedSize(
          (HUF_CElt const *)prevHuf->CTable, countWksp, maxSymbolValue);
      if (oldCSize < srcSize &&
          (oldCSize <= hSize + newCSize || hSize + 12 >= srcSize)) {
        DEBUGLOG(5, "set_repeat - smaller");
        ZSTD_memcpy(nextHuf, prevHuf, sizeof(*prevHuf));
        hufMetadata->hType = set_repeat;
        return 0;
      }
    }
    if (newCSize + hSize >= srcSize) {
      DEBUGLOG(5, "set_basic - no gains");
      ZSTD_memcpy(nextHuf, prevHuf, sizeof(*prevHuf));
      hufMetadata->hType = set_basic;
      return 0;
    }
    DEBUGLOG(5, "set_compressed (hSize=%u)", (U32)hSize);
    hufMetadata->hType = set_compressed;
    nextHuf->repeatMode = HUF_repeat_check;
    return hSize;
  }
}

static ZSTD_symbolEncodingTypeStats_t
ZSTD_buildDummySequencesStatistics(ZSTD_fseCTables_t *nextEntropy) {
  ZSTD_symbolEncodingTypeStats_t stats = {set_basic, set_basic, set_basic,
                                          0,         0,         0};
  nextEntropy->litlength_repeatMode = FSE_repeat_none;
  nextEntropy->offcode_repeatMode = FSE_repeat_none;
  nextEntropy->matchlength_repeatMode = FSE_repeat_none;
  return stats;
}

static size_t ZSTD_buildBlockEntropyStats_sequences(
    const SeqStore_t *seqStorePtr, const ZSTD_fseCTables_t *prevEntropy,
    ZSTD_fseCTables_t *nextEntropy, const ZSTD_CCtx_params *cctxParams,
    ZSTD_fseCTablesMetadata_t *fseMetadata, void *workspace, size_t wkspSize) {
  ZSTD_strategy const strategy = cctxParams->cParams.strategy;
  size_t const nbSeq =
      (size_t)(seqStorePtr->sequences - seqStorePtr->sequencesStart);
  BYTE *const ostart = fseMetadata->fseTablesBuffer;
  BYTE *const oend = ostart + sizeof(fseMetadata->fseTablesBuffer);
  BYTE *op = ostart;
  unsigned *countWorkspace = (unsigned *)workspace;
  unsigned *entropyWorkspace = countWorkspace + (MaxSeq + 1);
  size_t entropyWorkspaceSize =
      wkspSize - (MaxSeq + 1) * sizeof(*countWorkspace);
  ZSTD_symbolEncodingTypeStats_t stats;

  DEBUGLOG(5, "ZSTD_buildBlockEntropyStats_sequences (nbSeq=%zu)", nbSeq);
  stats = nbSeq != 0
              ? ZSTD_buildSequencesStatistics(seqStorePtr, nbSeq, prevEntropy,
                                              nextEntropy, op, oend, strategy,
                                              countWorkspace, entropyWorkspace,
                                              entropyWorkspaceSize)
              : ZSTD_buildDummySequencesStatistics(nextEntropy);
  FORWARD_IF_ERROR(stats.size, "ZSTD_buildSequencesStatistics failed!");
  fseMetadata->llType = (SymbolEncodingType_e)stats.LLtype;
  fseMetadata->ofType = (SymbolEncodingType_e)stats.Offtype;
  fseMetadata->mlType = (SymbolEncodingType_e)stats.MLtype;
  fseMetadata->lastCountSize = stats.lastCountSize;
  return stats.size;
}

size_t ZSTD_buildBlockEntropyStats(
    const SeqStore_t *seqStorePtr, const ZSTD_entropyCTables_t *prevEntropy,
    ZSTD_entropyCTables_t *nextEntropy, const ZSTD_CCtx_params *cctxParams,
    ZSTD_entropyCTablesMetadata_t *entropyMetadata, void *workspace,
    size_t wkspSize) {
  size_t const litSize = (size_t)(seqStorePtr->lit - seqStorePtr->litStart);
  int const huf_useOptDepth =
      (cctxParams->cParams.strategy >= HUF_OPTIMAL_DEPTH_THRESHOLD);
  int const hufFlags = huf_useOptDepth ? HUF_flags_optimalDepth : 0;

  entropyMetadata->hufMetadata.hufDesSize =
      ZSTD_buildBlockEntropyStats_literals(
          seqStorePtr->litStart, litSize, &prevEntropy->huf, &nextEntropy->huf,
          &entropyMetadata->hufMetadata,
          ZSTD_literalsCompressionIsDisabled(cctxParams), workspace, wkspSize,
          hufFlags);

  FORWARD_IF_ERROR(entropyMetadata->hufMetadata.hufDesSize,
                   "ZSTD_buildBlockEntropyStats_literals failed");
  entropyMetadata->fseMetadata.fseTablesSize =
      ZSTD_buildBlockEntropyStats_sequences(
          seqStorePtr, &prevEntropy->fse, &nextEntropy->fse, cctxParams,
          &entropyMetadata->fseMetadata, workspace, wkspSize);
  FORWARD_IF_ERROR(entropyMetadata->fseMetadata.fseTablesSize,
                   "ZSTD_buildBlockEntropyStats_sequences failed");
  return 0;
}

static size_t ZSTD_estimateBlockSize_literal(
    const BYTE *literals, size_t litSize, const ZSTD_hufCTables_t *huf,
    const ZSTD_hufCTablesMetadata_t *hufMetadata, void *workspace,
    size_t wkspSize, int writeEntropy) {
  unsigned *const countWksp = (unsigned *)workspace;
  unsigned maxSymbolValue = HUF_SYMBOLVALUE_MAX;
  size_t literalSectionHeaderSize = 3 + (litSize >= 1 KB) + (litSize >= 16 KB);
  U32 singleStream = litSize < 256;

  if (hufMetadata->hType == set_basic)
    return litSize;
  else if (hufMetadata->hType == set_rle)
    return 1;
  else if (hufMetadata->hType == set_compressed ||
           hufMetadata->hType == set_repeat) {
    size_t const largest =
        HIST_count_wksp(countWksp, &maxSymbolValue, (const BYTE *)literals,
                        litSize, workspace, wkspSize);
    if (ZSTD_isError(largest))
      return litSize;
    {
      size_t cLitSizeEstimate = HUF_estimateCompressedSize(
          (const HUF_CElt *)huf->CTable, countWksp, maxSymbolValue);
      if (writeEntropy)
        cLitSizeEstimate += hufMetadata->hufDesSize;
      if (!singleStream)
        cLitSizeEstimate += 6;
      return cLitSizeEstimate + literalSectionHeaderSize;
    }
  }
  assert(0);
  return 0;
}

static size_t ZSTD_estimateBlockSize_symbolType(
    SymbolEncodingType_e type, const BYTE *codeTable, size_t nbSeq,
    unsigned maxCode, const FSE_CTable *fseCTable, const U8 *additionalBits,
    short const *defaultNorm, U32 defaultNormLog, U32 defaultMax,
    void *workspace, size_t wkspSize) {
  unsigned *const countWksp = (unsigned *)workspace;
  const BYTE *ctp = codeTable;
  const BYTE *const ctStart = ctp;
  const BYTE *const ctEnd = ctStart + nbSeq;
  size_t cSymbolTypeSizeEstimateInBits = 0;
  unsigned max = maxCode;

  HIST_countFast_wksp(countWksp, &max, codeTable, nbSeq, workspace, wkspSize);
  if (type == set_basic) {

    assert(max <= defaultMax);
    (void)defaultMax;
    cSymbolTypeSizeEstimateInBits =
        ZSTD_crossEntropyCost(defaultNorm, defaultNormLog, countWksp, max);
  } else if (type == set_rle) {
    cSymbolTypeSizeEstimateInBits = 0;
  } else if (type == set_compressed || type == set_repeat) {
    cSymbolTypeSizeEstimateInBits = ZSTD_fseBitCost(fseCTable, countWksp, max);
  }
  if (ZSTD_isError(cSymbolTypeSizeEstimateInBits)) {
    return nbSeq * 10;
  }
  while (ctp < ctEnd) {
    if (additionalBits)
      cSymbolTypeSizeEstimateInBits += additionalBits[*ctp];
    else
      cSymbolTypeSizeEstimateInBits += *ctp;

    ctp++;
  }
  return cSymbolTypeSizeEstimateInBits >> 3;
}

static size_t ZSTD_estimateBlockSize_sequences(
    const BYTE *ofCodeTable, const BYTE *llCodeTable, const BYTE *mlCodeTable,
    size_t nbSeq, const ZSTD_fseCTables_t *fseTables,
    const ZSTD_fseCTablesMetadata_t *fseMetadata, void *workspace,
    size_t wkspSize, int writeEntropy) {
  size_t sequencesSectionHeaderSize =
      1 + 1 + (nbSeq >= 128) + (nbSeq >= LONGNBSEQ);
  size_t cSeqSizeEstimate = 0;
  cSeqSizeEstimate += ZSTD_estimateBlockSize_symbolType(
      fseMetadata->ofType, ofCodeTable, nbSeq, MaxOff, fseTables->offcodeCTable,
      NULL, OF_defaultNorm, OF_defaultNormLog, DefaultMaxOff, workspace,
      wkspSize);
  cSeqSizeEstimate += ZSTD_estimateBlockSize_symbolType(
      fseMetadata->llType, llCodeTable, nbSeq, MaxLL,
      fseTables->litlengthCTable, LL_bits, LL_defaultNorm, LL_defaultNormLog,
      MaxLL, workspace, wkspSize);
  cSeqSizeEstimate += ZSTD_estimateBlockSize_symbolType(
      fseMetadata->mlType, mlCodeTable, nbSeq, MaxML,
      fseTables->matchlengthCTable, ML_bits, ML_defaultNorm, ML_defaultNormLog,
      MaxML, workspace, wkspSize);
  if (writeEntropy)
    cSeqSizeEstimate += fseMetadata->fseTablesSize;
  return cSeqSizeEstimate + sequencesSectionHeaderSize;
}

static size_t ZSTD_estimateBlockSize(
    const BYTE *literals, size_t litSize, const BYTE *ofCodeTable,
    const BYTE *llCodeTable, const BYTE *mlCodeTable, size_t nbSeq,
    const ZSTD_entropyCTables_t *entropy,
    const ZSTD_entropyCTablesMetadata_t *entropyMetadata, void *workspace,
    size_t wkspSize, int writeLitEntropy, int writeSeqEntropy) {
  size_t const literalsSize = ZSTD_estimateBlockSize_literal(
      literals, litSize, &entropy->huf, &entropyMetadata->hufMetadata,
      workspace, wkspSize, writeLitEntropy);
  size_t const seqSize = ZSTD_estimateBlockSize_sequences(
      ofCodeTable, llCodeTable, mlCodeTable, nbSeq, &entropy->fse,
      &entropyMetadata->fseMetadata, workspace, wkspSize, writeSeqEntropy);
  return seqSize + literalsSize + ZSTD_blockHeaderSize;
}

static size_t
ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize(SeqStore_t *seqStore,
                                                   ZSTD_CCtx *zc) {
  ZSTD_entropyCTablesMetadata_t *const entropyMetadata =
      &zc->blockSplitCtx.entropyMetadata;
  DEBUGLOG(6, "ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize()");
  FORWARD_IF_ERROR(ZSTD_buildBlockEntropyStats(
                       seqStore, &zc->blockState.prevCBlock->entropy,
                       &zc->blockState.nextCBlock->entropy, &zc->appliedParams,
                       entropyMetadata, zc->tmpWorkspace, zc->tmpWkspSize),
                   "");
  return ZSTD_estimateBlockSize(
      seqStore->litStart, (size_t)(seqStore->lit - seqStore->litStart),
      seqStore->ofCode, seqStore->llCode, seqStore->mlCode,
      (size_t)(seqStore->sequences - seqStore->sequencesStart),
      &zc->blockState.nextCBlock->entropy, entropyMetadata, zc->tmpWorkspace,
      zc->tmpWkspSize,
      (int)(entropyMetadata->hufMetadata.hType == set_compressed), 1);
}

static size_t
ZSTD_countSeqStoreLiteralsBytes(const SeqStore_t *const seqStore) {
  size_t literalsBytes = 0;
  size_t const nbSeqs =
      (size_t)(seqStore->sequences - seqStore->sequencesStart);
  size_t i;
  for (i = 0; i < nbSeqs; ++i) {
    SeqDef const seq = seqStore->sequencesStart[i];
    literalsBytes += seq.litLength;
    if (i == seqStore->longLengthPos &&
        seqStore->longLengthType == ZSTD_llt_literalLength) {
      literalsBytes += 0x10000;
    }
  }
  return literalsBytes;
}

static size_t ZSTD_countSeqStoreMatchBytes(const SeqStore_t *const seqStore) {
  size_t matchBytes = 0;
  size_t const nbSeqs =
      (size_t)(seqStore->sequences - seqStore->sequencesStart);
  size_t i;
  for (i = 0; i < nbSeqs; ++i) {
    SeqDef seq = seqStore->sequencesStart[i];
    matchBytes += seq.mlBase + MINMATCH;
    if (i == seqStore->longLengthPos &&
        seqStore->longLengthType == ZSTD_llt_matchLength) {
      matchBytes += 0x10000;
    }
  }
  return matchBytes;
}

static void ZSTD_deriveSeqStoreChunk(SeqStore_t *resultSeqStore,
                                     const SeqStore_t *originalSeqStore,
                                     size_t startIdx, size_t endIdx) {
  *resultSeqStore = *originalSeqStore;
  if (startIdx > 0) {
    resultSeqStore->sequences = originalSeqStore->sequencesStart + startIdx;
    resultSeqStore->litStart += ZSTD_countSeqStoreLiteralsBytes(resultSeqStore);
  }

  if (originalSeqStore->longLengthType != ZSTD_llt_none) {
    if (originalSeqStore->longLengthPos < startIdx ||
        originalSeqStore->longLengthPos > endIdx) {
      resultSeqStore->longLengthType = ZSTD_llt_none;
    } else {
      resultSeqStore->longLengthPos -= (U32)startIdx;
    }
  }
  resultSeqStore->sequencesStart = originalSeqStore->sequencesStart + startIdx;
  resultSeqStore->sequences = originalSeqStore->sequencesStart + endIdx;
  if (endIdx == (size_t)(originalSeqStore->sequences -
                         originalSeqStore->sequencesStart)) {

    assert(resultSeqStore->lit == originalSeqStore->lit);
  } else {
    size_t const literalsBytes =
        ZSTD_countSeqStoreLiteralsBytes(resultSeqStore);
    resultSeqStore->lit = resultSeqStore->litStart + literalsBytes;
  }
  resultSeqStore->llCode += startIdx;
  resultSeqStore->mlCode += startIdx;
  resultSeqStore->ofCode += startIdx;
}

static U32 ZSTD_resolveRepcodeToRawOffset(const U32 rep[ZSTD_REP_NUM],
                                          const U32 offBase, const U32 ll0) {
  U32 const adjustedRepCode = OFFBASE_TO_REPCODE(offBase) - 1 + ll0;
  assert(OFFBASE_IS_REPCODE(offBase));
  if (adjustedRepCode == ZSTD_REP_NUM) {
    assert(ll0);

    return rep[0] - 1;
  }
  return rep[adjustedRepCode];
}

static void ZSTD_seqStore_resolveOffCodes(Repcodes_t *const dRepcodes,
                                          Repcodes_t *const cRepcodes,
                                          const SeqStore_t *const seqStore,
                                          U32 const nbSeq) {
  U32 idx = 0;
  U32 const longLitLenIdx = seqStore->longLengthType == ZSTD_llt_literalLength
                                ? seqStore->longLengthPos
                                : nbSeq;
  for (; idx < nbSeq; ++idx) {
    SeqDef *const seq = seqStore->sequencesStart + idx;
    U32 const ll0 = (seq->litLength == 0) && (idx != longLitLenIdx);
    U32 const offBase = seq->offBase;
    assert(offBase > 0);
    if (OFFBASE_IS_REPCODE(offBase)) {
      U32 const dRawOffset =
          ZSTD_resolveRepcodeToRawOffset(dRepcodes->rep, offBase, ll0);
      U32 const cRawOffset =
          ZSTD_resolveRepcodeToRawOffset(cRepcodes->rep, offBase, ll0);

      if (dRawOffset != cRawOffset) {
        seq->offBase = OFFSET_TO_OFFBASE(cRawOffset);
      }
    }

    ZSTD_updateRep(dRepcodes->rep, seq->offBase, ll0);
    ZSTD_updateRep(cRepcodes->rep, offBase, ll0);
  }
}

static size_t ZSTD_compressSeqStore_singleBlock(
    ZSTD_CCtx *zc, const SeqStore_t *const seqStore, Repcodes_t *const dRep,
    Repcodes_t *const cRep, void *dst, size_t dstCapacity, const void *src,
    size_t srcSize, U32 lastBlock, U32 isPartition) {
  const U32 rleMaxLength = 25;
  BYTE *op = (BYTE *)dst;
  const BYTE *ip = (const BYTE *)src;
  size_t cSize;
  size_t cSeqsSize;

  Repcodes_t const dRepOriginal = *dRep;
  DEBUGLOG(5, "ZSTD_compressSeqStore_singleBlock");
  if (isPartition)
    ZSTD_seqStore_resolveOffCodes(
        dRep, cRep, seqStore,
        (U32)(seqStore->sequences - seqStore->sequencesStart));

  RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize, dstSize_tooSmall,
                  "Block header doesn't fit");
  cSeqsSize = ZSTD_entropyCompressSeqStore(
      seqStore, &zc->blockState.prevCBlock->entropy,
      &zc->blockState.nextCBlock->entropy, &zc->appliedParams,
      op + ZSTD_blockHeaderSize, dstCapacity - ZSTD_blockHeaderSize, srcSize,
      zc->tmpWorkspace, zc->tmpWkspSize, zc->bmi2);
  FORWARD_IF_ERROR(cSeqsSize, "ZSTD_entropyCompressSeqStore failed!");

  if (!zc->isFirstBlock && cSeqsSize < rleMaxLength &&
      ZSTD_isRLE((BYTE const *)src, srcSize)) {

    cSeqsSize = 1;
  }

  if (zc->seqCollector.collectSequences) {
    FORWARD_IF_ERROR(
        ZSTD_copyBlockSequences(&zc->seqCollector, seqStore, dRepOriginal.rep),
        "copyBlockSequences failed");
    ZSTD_blockState_confirmRepcodesAndEntropyTables(&zc->blockState);
    return 0;
  }

  if (cSeqsSize == 0) {
    cSize = ZSTD_noCompressBlock(op, dstCapacity, ip, srcSize, lastBlock);
    FORWARD_IF_ERROR(cSize, "Nocompress block failed");
    DEBUGLOG(5, "Writing out nocompress block, size: %zu", cSize);
    *dRep = dRepOriginal;
  } else if (cSeqsSize == 1) {
    cSize = ZSTD_rleCompressBlock(op, dstCapacity, *ip, srcSize, lastBlock);
    FORWARD_IF_ERROR(cSize, "RLE compress block failed");
    DEBUGLOG(5, "Writing out RLE block, size: %zu", cSize);
    *dRep = dRepOriginal;
  } else {
    ZSTD_blockState_confirmRepcodesAndEntropyTables(&zc->blockState);
    writeBlockHeader(op, cSeqsSize, srcSize, lastBlock);
    cSize = ZSTD_blockHeaderSize + cSeqsSize;
    DEBUGLOG(5, "Writing out compressed block, size: %zu", cSize);
  }

  if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
      FSE_repeat_valid)
    zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
        FSE_repeat_check;

  return cSize;
}

typedef struct {
  U32 *splitLocations;
  size_t idx;
} seqStoreSplits;

#define MIN_SEQUENCES_BLOCK_SPLITTING 300

static void ZSTD_deriveBlockSplitsHelper(seqStoreSplits *splits,
                                         size_t startIdx, size_t endIdx,
                                         ZSTD_CCtx *zc,
                                         const SeqStore_t *origSeqStore) {
  SeqStore_t *const fullSeqStoreChunk = &zc->blockSplitCtx.fullSeqStoreChunk;
  SeqStore_t *const firstHalfSeqStore = &zc->blockSplitCtx.firstHalfSeqStore;
  SeqStore_t *const secondHalfSeqStore = &zc->blockSplitCtx.secondHalfSeqStore;
  size_t estimatedOriginalSize;
  size_t estimatedFirstHalfSize;
  size_t estimatedSecondHalfSize;
  size_t midIdx = (startIdx + endIdx) / 2;

  DEBUGLOG(5, "ZSTD_deriveBlockSplitsHelper: startIdx=%zu endIdx=%zu", startIdx,
           endIdx);
  assert(endIdx >= startIdx);
  if (endIdx - startIdx < MIN_SEQUENCES_BLOCK_SPLITTING ||
      splits->idx >= ZSTD_MAX_NB_BLOCK_SPLITS) {
    DEBUGLOG(6, "ZSTD_deriveBlockSplitsHelper: Too few sequences (%zu)",
             endIdx - startIdx);
    return;
  }
  ZSTD_deriveSeqStoreChunk(fullSeqStoreChunk, origSeqStore, startIdx, endIdx);
  ZSTD_deriveSeqStoreChunk(firstHalfSeqStore, origSeqStore, startIdx, midIdx);
  ZSTD_deriveSeqStoreChunk(secondHalfSeqStore, origSeqStore, midIdx, endIdx);
  estimatedOriginalSize =
      ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize(fullSeqStoreChunk, zc);
  estimatedFirstHalfSize =
      ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize(firstHalfSeqStore, zc);
  estimatedSecondHalfSize = ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize(
      secondHalfSeqStore, zc);
  DEBUGLOG(5,
           "Estimated original block size: %zu -- First half split: %zu -- "
           "Second half split: %zu",
           estimatedOriginalSize, estimatedFirstHalfSize,
           estimatedSecondHalfSize);
  if (ZSTD_isError(estimatedOriginalSize) ||
      ZSTD_isError(estimatedFirstHalfSize) ||
      ZSTD_isError(estimatedSecondHalfSize)) {
    return;
  }
  if (estimatedFirstHalfSize + estimatedSecondHalfSize <
      estimatedOriginalSize) {
    DEBUGLOG(5, "split decided at seqNb:%zu", midIdx);
    ZSTD_deriveBlockSplitsHelper(splits, startIdx, midIdx, zc, origSeqStore);
    splits->splitLocations[splits->idx] = (U32)midIdx;
    splits->idx++;
    ZSTD_deriveBlockSplitsHelper(splits, midIdx, endIdx, zc, origSeqStore);
  }
}

static size_t ZSTD_deriveBlockSplits(ZSTD_CCtx *zc, U32 partitions[],
                                     U32 nbSeq) {
  seqStoreSplits splits;
  splits.splitLocations = partitions;
  splits.idx = 0;
  if (nbSeq <= 4) {
    DEBUGLOG(5, "ZSTD_deriveBlockSplits: Too few sequences to split (%u <= 4)",
             nbSeq);

    return 0;
  }
  ZSTD_deriveBlockSplitsHelper(&splits, 0, nbSeq, zc, &zc->seqStore);
  splits.splitLocations[splits.idx] = nbSeq;
  DEBUGLOG(5, "ZSTD_deriveBlockSplits: final nb partitions: %zu",
           splits.idx + 1);
  return splits.idx;
}

static size_t ZSTD_compressBlock_splitBlock_internal(ZSTD_CCtx *zc, void *dst,
                                                     size_t dstCapacity,
                                                     const void *src,
                                                     size_t blockSize,
                                                     U32 lastBlock, U32 nbSeq) {
  size_t cSize = 0;
  const BYTE *ip = (const BYTE *)src;
  BYTE *op = (BYTE *)dst;
  size_t i = 0;
  size_t srcBytesTotal = 0;
  U32 *const partitions = zc->blockSplitCtx.partitions;
  SeqStore_t *const nextSeqStore = &zc->blockSplitCtx.nextSeqStore;
  SeqStore_t *const currSeqStore = &zc->blockSplitCtx.currSeqStore;
  size_t const numSplits = ZSTD_deriveBlockSplits(zc, partitions, nbSeq);

  Repcodes_t dRep;
  Repcodes_t cRep;
  ZSTD_memcpy(dRep.rep, zc->blockState.prevCBlock->rep, sizeof(Repcodes_t));
  ZSTD_memcpy(cRep.rep, zc->blockState.prevCBlock->rep, sizeof(Repcodes_t));
  ZSTD_memset(nextSeqStore, 0, sizeof(SeqStore_t));

  DEBUGLOG(5,
           "ZSTD_compressBlock_splitBlock_internal (dstCapacity=%u, "
           "dictLimit=%u, nextToUpdate=%u)",
           (unsigned)dstCapacity,
           (unsigned)zc->blockState.matchState.window.dictLimit,
           (unsigned)zc->blockState.matchState.nextToUpdate);

  if (numSplits == 0) {
    size_t cSizeSingleBlock = ZSTD_compressSeqStore_singleBlock(
        zc, &zc->seqStore, &dRep, &cRep, op, dstCapacity, ip, blockSize,
        lastBlock, 0);
    FORWARD_IF_ERROR(
        cSizeSingleBlock,
        "Compressing single block from splitBlock_internal() failed!");
    DEBUGLOG(5, "ZSTD_compressBlock_splitBlock_internal: No splits");
    assert(zc->blockSizeMax <= ZSTD_BLOCKSIZE_MAX);
    assert(cSizeSingleBlock <= zc->blockSizeMax + ZSTD_blockHeaderSize);
    return cSizeSingleBlock;
  }

  ZSTD_deriveSeqStoreChunk(currSeqStore, &zc->seqStore, 0, partitions[0]);
  for (i = 0; i <= numSplits; ++i) {
    size_t cSizeChunk;
    U32 const lastPartition = (i == numSplits);
    U32 lastBlockEntireSrc = 0;

    size_t srcBytes = ZSTD_countSeqStoreLiteralsBytes(currSeqStore) +
                      ZSTD_countSeqStoreMatchBytes(currSeqStore);
    srcBytesTotal += srcBytes;
    if (lastPartition) {

      srcBytes += blockSize - srcBytesTotal;
      lastBlockEntireSrc = lastBlock;
    } else {
      ZSTD_deriveSeqStoreChunk(nextSeqStore, &zc->seqStore, partitions[i],
                               partitions[i + 1]);
    }

    cSizeChunk = ZSTD_compressSeqStore_singleBlock(
        zc, currSeqStore, &dRep, &cRep, op, dstCapacity, ip, srcBytes,
        lastBlockEntireSrc, 1);
    DEBUGLOG(
        5, "Estimated size: %zu vs %zu : actual size",
        ZSTD_buildEntropyStatisticsAndEstimateSubBlockSize(currSeqStore, zc),
        cSizeChunk);
    FORWARD_IF_ERROR(cSizeChunk, "Compressing chunk failed!");

    ip += srcBytes;
    op += cSizeChunk;
    dstCapacity -= cSizeChunk;
    cSize += cSizeChunk;
    *currSeqStore = *nextSeqStore;
    assert(cSizeChunk <= zc->blockSizeMax + ZSTD_blockHeaderSize);
  }

  ZSTD_memcpy(zc->blockState.prevCBlock->rep, dRep.rep, sizeof(Repcodes_t));
  return cSize;
}

static size_t ZSTD_compressBlock_splitBlock(ZSTD_CCtx *zc, void *dst,
                                            size_t dstCapacity, const void *src,
                                            size_t srcSize, U32 lastBlock) {
  U32 nbSeq;
  size_t cSize;
  DEBUGLOG(5, "ZSTD_compressBlock_splitBlock");
  assert(zc->appliedParams.postBlockSplitter == ZSTD_ps_enable);

  {
    const size_t bss = ZSTD_buildSeqStore(zc, src, srcSize);
    FORWARD_IF_ERROR(bss, "ZSTD_buildSeqStore failed");
    if (bss == ZSTDbss_noCompress) {
      if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
          FSE_repeat_valid)
        zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
            FSE_repeat_check;
      RETURN_ERROR_IF(zc->seqCollector.collectSequences,
                      sequenceProducer_failed, "Uncompressible block");
      cSize = ZSTD_noCompressBlock(dst, dstCapacity, src, srcSize, lastBlock);
      FORWARD_IF_ERROR(cSize, "ZSTD_noCompressBlock failed");
      DEBUGLOG(5, "ZSTD_compressBlock_splitBlock: Nocompress block");
      return cSize;
    }
    nbSeq = (U32)(zc->seqStore.sequences - zc->seqStore.sequencesStart);
  }

  cSize = ZSTD_compressBlock_splitBlock_internal(zc, dst, dstCapacity, src,
                                                 srcSize, lastBlock, nbSeq);
  FORWARD_IF_ERROR(cSize, "Splitting blocks failed!");
  return cSize;
}

static size_t ZSTD_compressBlock_internal(ZSTD_CCtx *zc, void *dst,
                                          size_t dstCapacity, const void *src,
                                          size_t srcSize, U32 frame) {

  const U32 rleMaxLength = 25;
  size_t cSize;
  const BYTE *ip = (const BYTE *)src;
  BYTE *op = (BYTE *)dst;
  DEBUGLOG(5,
           "ZSTD_compressBlock_internal (dstCapacity=%u, dictLimit=%u, "
           "nextToUpdate=%u)",
           (unsigned)dstCapacity,
           (unsigned)zc->blockState.matchState.window.dictLimit,
           (unsigned)zc->blockState.matchState.nextToUpdate);

  {
    const size_t bss = ZSTD_buildSeqStore(zc, src, srcSize);
    FORWARD_IF_ERROR(bss, "ZSTD_buildSeqStore failed");
    if (bss == ZSTDbss_noCompress) {
      RETURN_ERROR_IF(zc->seqCollector.collectSequences,
                      sequenceProducer_failed, "Uncompressible block");
      cSize = 0;
      goto out;
    }
  }

  if (zc->seqCollector.collectSequences) {
    FORWARD_IF_ERROR(ZSTD_copyBlockSequences(&zc->seqCollector,
                                             ZSTD_getSeqStore(zc),
                                             zc->blockState.prevCBlock->rep),
                     "copyBlockSequences failed");
    ZSTD_blockState_confirmRepcodesAndEntropyTables(&zc->blockState);
    return 0;
  }

  cSize = ZSTD_entropyCompressSeqStore(
      &zc->seqStore, &zc->blockState.prevCBlock->entropy,
      &zc->blockState.nextCBlock->entropy, &zc->appliedParams, dst, dstCapacity,
      srcSize, zc->tmpWorkspace, zc->tmpWkspSize, zc->bmi2);

  if (frame &&

      !zc->isFirstBlock && cSize < rleMaxLength && ZSTD_isRLE(ip, srcSize)) {
    cSize = 1;
    op[0] = ip[0];
  }

out:
  if (!ZSTD_isError(cSize) && cSize > 1) {
    ZSTD_blockState_confirmRepcodesAndEntropyTables(&zc->blockState);
  }

  if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
      FSE_repeat_valid)
    zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
        FSE_repeat_check;

  return cSize;
}

static size_t ZSTD_compressBlock_targetCBlockSize_body(
    ZSTD_CCtx *zc, void *dst, size_t dstCapacity, const void *src,
    size_t srcSize, const size_t bss, U32 lastBlock) {
  DEBUGLOG(6, "Attempting ZSTD_compressSuperBlock()");
  if (bss == ZSTDbss_compress) {
    if (

        !zc->isFirstBlock && ZSTD_maybeRLE(&zc->seqStore) &&
        ZSTD_isRLE((BYTE const *)src, srcSize)) {
      return ZSTD_rleCompressBlock(dst, dstCapacity, *(BYTE const *)src,
                                   srcSize, lastBlock);
    }

    {
      size_t const cSize = ZSTD_compressSuperBlock(zc, dst, dstCapacity, src,
                                                   srcSize, lastBlock);
      if (cSize != ERROR(dstSize_tooSmall)) {
        size_t const maxCSize =
            srcSize - ZSTD_minGain(srcSize, zc->appliedParams.cParams.strategy);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressSuperBlock failed");
        if (cSize != 0 && cSize < maxCSize + ZSTD_blockHeaderSize) {
          ZSTD_blockState_confirmRepcodesAndEntropyTables(&zc->blockState);
          return cSize;
        }
      }
    }
  }

  DEBUGLOG(6, "Resorting to ZSTD_noCompressBlock()");

  return ZSTD_noCompressBlock(dst, dstCapacity, src, srcSize, lastBlock);
}

static size_t ZSTD_compressBlock_targetCBlockSize(ZSTD_CCtx *zc, void *dst,
                                                  size_t dstCapacity,
                                                  const void *src,
                                                  size_t srcSize,
                                                  U32 lastBlock) {
  size_t cSize = 0;
  const size_t bss = ZSTD_buildSeqStore(zc, src, srcSize);
  DEBUGLOG(5,
           "ZSTD_compressBlock_targetCBlockSize (dstCapacity=%u, dictLimit=%u, "
           "nextToUpdate=%u, srcSize=%zu)",
           (unsigned)dstCapacity,
           (unsigned)zc->blockState.matchState.window.dictLimit,
           (unsigned)zc->blockState.matchState.nextToUpdate, srcSize);
  FORWARD_IF_ERROR(bss, "ZSTD_buildSeqStore failed");

  cSize = ZSTD_compressBlock_targetCBlockSize_body(zc, dst, dstCapacity, src,
                                                   srcSize, bss, lastBlock);
  FORWARD_IF_ERROR(cSize, "ZSTD_compressBlock_targetCBlockSize_body failed");

  if (zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
      FSE_repeat_valid)
    zc->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
        FSE_repeat_check;

  return cSize;
}

static void ZSTD_overflowCorrectIfNeeded(ZSTD_MatchState_t *ms, ZSTD_cwksp *ws,
                                         ZSTD_CCtx_params const *params,
                                         void const *ip, void const *iend) {
  U32 const cycleLog =
      ZSTD_cycleLog(params->cParams.chainLog, params->cParams.strategy);
  U32 const maxDist = (U32)1 << params->cParams.windowLog;
  if (ZSTD_window_needOverflowCorrection(ms->window, cycleLog, maxDist,
                                         ms->loadedDictEnd, ip, iend)) {
    U32 const correction =
        ZSTD_window_correctOverflow(&ms->window, cycleLog, maxDist, ip);
    ZSTD_STATIC_ASSERT(ZSTD_CHAINLOG_MAX <= 30);
    ZSTD_STATIC_ASSERT(ZSTD_WINDOWLOG_MAX_32 <= 30);
    ZSTD_STATIC_ASSERT(ZSTD_WINDOWLOG_MAX <= 31);
    ZSTD_cwksp_mark_tables_dirty(ws);
    ZSTD_reduceIndex(ms, params, correction);
    ZSTD_cwksp_mark_tables_clean(ws);
    if (ms->nextToUpdate < correction)
      ms->nextToUpdate = 0;
    else
      ms->nextToUpdate -= correction;

    ms->loadedDictEnd = 0;
    ms->dictMatchState = NULL;
  }
}

#include "zstd_preSplit.h"

static size_t ZSTD_optimalBlockSize(ZSTD_CCtx *cctx, const void *src,
                                    size_t srcSize, size_t blockSizeMax,
                                    int splitLevel, ZSTD_strategy strat,
                                    S64 savings) {

  static const int splitLevels[] = {0, 0, 1, 2, 2, 3, 3, 4, 4, 4};

  if (srcSize < 128 KB || blockSizeMax < 128 KB)
    return MIN(srcSize, blockSizeMax);

  if (savings < 3) {
    DEBUGLOG(6, "don't attempt splitting: savings (%i) too low", (int)savings);
    return 128 KB;
  }

  if (splitLevel == 1)
    return 128 KB;
  if (splitLevel == 0) {
    assert(ZSTD_fast <= strat && strat <= ZSTD_btultra2);
    splitLevel = splitLevels[strat];
  } else {
    assert(2 <= splitLevel && splitLevel <= 6);
    splitLevel -= 2;
  }
  return ZSTD_splitBlock(src, blockSizeMax, splitLevel, cctx->tmpWorkspace,
                         cctx->tmpWkspSize);
}

static size_t ZSTD_compress_frameChunk(ZSTD_CCtx *cctx, void *dst,
                                       size_t dstCapacity, const void *src,
                                       size_t srcSize, U32 lastFrameChunk) {
  size_t blockSizeMax = cctx->blockSizeMax;
  size_t remaining = srcSize;
  const BYTE *ip = (const BYTE *)src;
  BYTE *const ostart = (BYTE *)dst;
  BYTE *op = ostart;
  U32 const maxDist = (U32)1 << cctx->appliedParams.cParams.windowLog;
  S64 savings = (S64)cctx->consumedSrcSize - (S64)cctx->producedCSize;

  assert(cctx->appliedParams.cParams.windowLog <= ZSTD_WINDOWLOG_MAX);

  DEBUGLOG(5, "ZSTD_compress_frameChunk (srcSize=%u, blockSizeMax=%u)",
           (unsigned)srcSize, (unsigned)blockSizeMax);
  if (cctx->appliedParams.fParams.checksumFlag && srcSize)
    XXH64_update(&cctx->xxhState, src, srcSize);

  while (remaining) {
    ZSTD_MatchState_t *const ms = &cctx->blockState.matchState;
    size_t const blockSize =
        ZSTD_optimalBlockSize(cctx, ip, remaining, blockSizeMax,
                              cctx->appliedParams.preBlockSplitter_level,
                              cctx->appliedParams.cParams.strategy, savings);
    U32 const lastBlock = lastFrameChunk & (blockSize == remaining);
    assert(blockSize <= remaining);

    RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize + MIN_CBLOCK_SIZE + 1,
                    dstSize_tooSmall,
                    "not enough space to store compressed block");

    ZSTD_overflowCorrectIfNeeded(ms, &cctx->workspace, &cctx->appliedParams, ip,
                                 ip + blockSize);
    ZSTD_checkDictValidity(&ms->window, ip + blockSize, maxDist,
                           &ms->loadedDictEnd, &ms->dictMatchState);
    ZSTD_window_enforceMaxDist(&ms->window, ip, maxDist, &ms->loadedDictEnd,
                               &ms->dictMatchState);

    if (ms->nextToUpdate < ms->window.lowLimit)
      ms->nextToUpdate = ms->window.lowLimit;

    {
      size_t cSize;
      if (ZSTD_useTargetCBlockSize(&cctx->appliedParams)) {
        cSize = ZSTD_compressBlock_targetCBlockSize(cctx, op, dstCapacity, ip,
                                                    blockSize, lastBlock);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressBlock_targetCBlockSize failed");
        assert(cSize > 0);
        assert(cSize <= blockSize + ZSTD_blockHeaderSize);
      } else if (ZSTD_blockSplitterEnabled(&cctx->appliedParams)) {
        cSize = ZSTD_compressBlock_splitBlock(cctx, op, dstCapacity, ip,
                                              blockSize, lastBlock);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressBlock_splitBlock failed");
        assert(cSize > 0 || cctx->seqCollector.collectSequences == 1);
      } else {
        cSize = ZSTD_compressBlock_internal(cctx, op + ZSTD_blockHeaderSize,
                                            dstCapacity - ZSTD_blockHeaderSize,
                                            ip, blockSize, 1);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressBlock_internal failed");

        if (cSize == 0) {
          cSize =
              ZSTD_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
          FORWARD_IF_ERROR(cSize, "ZSTD_noCompressBlock failed");
        } else {
          U32 const cBlockHeader =
              cSize == 1
                  ? lastBlock + (((U32)bt_rle) << 1) + (U32)(blockSize << 3)
                  : lastBlock + (((U32)bt_compressed) << 1) + (U32)(cSize << 3);
          MEM_writeLE24(op, cBlockHeader);
          cSize += ZSTD_blockHeaderSize;
        }
      }

      savings += (S64)blockSize - (S64)cSize;

      ip += blockSize;
      assert(remaining >= blockSize);
      remaining -= blockSize;
      op += cSize;
      assert(dstCapacity >= cSize);
      dstCapacity -= cSize;
      cctx->isFirstBlock = 0;
      DEBUGLOG(5, "ZSTD_compress_frameChunk: adding a block of size %u",
               (unsigned)cSize);
    }
  }

  if (lastFrameChunk && (op > ostart))
    cctx->stage = ZSTDcs_ending;
  return (size_t)(op - ostart);
}

static size_t ZSTD_writeFrameHeader(void *dst, size_t dstCapacity,
                                    const ZSTD_CCtx_params *params,
                                    U64 pledgedSrcSize, U32 dictID) {
  BYTE *const op = (BYTE *)dst;
  U32 const dictIDSizeCodeLength =
      (dictID > 0) + (dictID >= 256) + (dictID >= 65536);
  U32 const dictIDSizeCode =
      params->fParams.noDictIDFlag ? 0 : dictIDSizeCodeLength;
  U32 const checksumFlag = params->fParams.checksumFlag > 0;
  U32 const windowSize = (U32)1 << params->cParams.windowLog;
  U32 const singleSegment =
      params->fParams.contentSizeFlag && (windowSize >= pledgedSrcSize);
  BYTE const windowLogByte =
      (BYTE)((params->cParams.windowLog - ZSTD_WINDOWLOG_ABSOLUTEMIN) << 3);
  U32 const fcsCode = params->fParams.contentSizeFlag
                          ? (pledgedSrcSize >= 256) +
                                (pledgedSrcSize >= 65536 + 256) +
                                (pledgedSrcSize >= 0xFFFFFFFFU)
                          : 0;
  BYTE const frameHeaderDescriptionByte =
      (BYTE)(dictIDSizeCode + (checksumFlag << 2) + (singleSegment << 5) +
             (fcsCode << 6));
  size_t pos = 0;

  assert(!(params->fParams.contentSizeFlag &&
           pledgedSrcSize == ZSTD_CONTENTSIZE_UNKNOWN));
  RETURN_ERROR_IF(dstCapacity < ZSTD_FRAMEHEADERSIZE_MAX, dstSize_tooSmall,
                  "dst buf is too small to fit worst-case frame header size.");
  DEBUGLOG(4,
           "ZSTD_writeFrameHeader : dictIDFlag : %u ; dictID : %u ; "
           "dictIDSizeCode : %u",
           !params->fParams.noDictIDFlag, (unsigned)dictID,
           (unsigned)dictIDSizeCode);
  if (params->format == ZSTD_f_zstd1) {
    MEM_writeLE32(dst, ZSTD_MAGICNUMBER);
    pos = 4;
  }
  op[pos++] = frameHeaderDescriptionByte;
  if (!singleSegment)
    op[pos++] = windowLogByte;
  switch (dictIDSizeCode) {
  default:
    assert(0);
    ZSTD_FALLTHROUGH;
  case 0:
    break;
  case 1:
    op[pos] = (BYTE)(dictID);
    pos++;
    break;
  case 2:
    MEM_writeLE16(op + pos, (U16)dictID);
    pos += 2;
    break;
  case 3:
    MEM_writeLE32(op + pos, dictID);
    pos += 4;
    break;
  }
  switch (fcsCode) {
  default:
    assert(0);
    ZSTD_FALLTHROUGH;
  case 0:
    if (singleSegment)
      op[pos++] = (BYTE)(pledgedSrcSize);
    break;
  case 1:
    MEM_writeLE16(op + pos, (U16)(pledgedSrcSize - 256));
    pos += 2;
    break;
  case 2:
    MEM_writeLE32(op + pos, (U32)(pledgedSrcSize));
    pos += 4;
    break;
  case 3:
    MEM_writeLE64(op + pos, (U64)(pledgedSrcSize));
    pos += 8;
    break;
  }
  return pos;
}

size_t ZSTD_writeSkippableFrame(void *dst, size_t dstCapacity, const void *src,
                                size_t srcSize, unsigned magicVariant) {
  BYTE *op = (BYTE *)dst;
  RETURN_ERROR_IF(dstCapacity < srcSize + ZSTD_SKIPPABLEHEADERSIZE,
                  dstSize_tooSmall, "Not enough room for skippable frame");
  RETURN_ERROR_IF(srcSize > (unsigned)0xFFFFFFFF, srcSize_wrong,
                  "Src size too large for skippable frame");
  RETURN_ERROR_IF(magicVariant > 15, parameter_outOfBound,
                  "Skippable frame magic number variant not supported");

  MEM_writeLE32(op, (U32)(ZSTD_MAGIC_SKIPPABLE_START + magicVariant));
  MEM_writeLE32(op + 4, (U32)srcSize);
  ZSTD_memcpy(op + 8, src, srcSize);
  return srcSize + ZSTD_SKIPPABLEHEADERSIZE;
}

size_t ZSTD_writeLastEmptyBlock(void *dst, size_t dstCapacity) {
  RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize, dstSize_tooSmall,
                  "dst buf is too small to write frame trailer empty block.");
  {
    U32 const cBlockHeader24 = 1 + (((U32)bt_raw) << 1);
    MEM_writeLE24(dst, cBlockHeader24);
    return ZSTD_blockHeaderSize;
  }
}

void ZSTD_referenceExternalSequences(ZSTD_CCtx *cctx, rawSeq *seq,
                                     size_t nbSeq) {
  assert(cctx->stage == ZSTDcs_init);
  assert(nbSeq == 0 ||
         cctx->appliedParams.ldmParams.enableLdm != ZSTD_ps_enable);
  cctx->externSeqStore.seq = seq;
  cctx->externSeqStore.size = nbSeq;
  cctx->externSeqStore.capacity = nbSeq;
  cctx->externSeqStore.pos = 0;
  cctx->externSeqStore.posInSequence = 0;
}

static size_t ZSTD_compressContinue_internal(ZSTD_CCtx *cctx, void *dst,
                                             size_t dstCapacity,
                                             const void *src, size_t srcSize,
                                             U32 frame, U32 lastFrameChunk) {
  ZSTD_MatchState_t *const ms = &cctx->blockState.matchState;
  size_t fhSize = 0;

  DEBUGLOG(5, "ZSTD_compressContinue_internal, stage: %u, srcSize: %u",
           cctx->stage, (unsigned)srcSize);
  RETURN_ERROR_IF(cctx->stage == ZSTDcs_created, stage_wrong,
                  "missing init (ZSTD_compressBegin)");

  if (frame && (cctx->stage == ZSTDcs_init)) {
    fhSize =
        ZSTD_writeFrameHeader(dst, dstCapacity, &cctx->appliedParams,
                              cctx->pledgedSrcSizePlusOne - 1, cctx->dictID);
    FORWARD_IF_ERROR(fhSize, "ZSTD_writeFrameHeader failed");
    assert(fhSize <= dstCapacity);
    dstCapacity -= fhSize;
    dst = (char *)dst + fhSize;
    cctx->stage = ZSTDcs_ongoing;
  }

  if (!srcSize)
    return fhSize;

  if (!ZSTD_window_update(&ms->window, src, srcSize, ms->forceNonContiguous)) {
    ms->forceNonContiguous = 0;
    ms->nextToUpdate = ms->window.dictLimit;
  }
  if (cctx->appliedParams.ldmParams.enableLdm == ZSTD_ps_enable) {
    ZSTD_window_update(&cctx->ldmState.window, src, srcSize, 0);
  }

  if (!frame) {

    ZSTD_overflowCorrectIfNeeded(ms, &cctx->workspace, &cctx->appliedParams,
                                 src, (BYTE const *)src + srcSize);
  }

  DEBUGLOG(5, "ZSTD_compressContinue_internal (blockSize=%u)",
           (unsigned)cctx->blockSizeMax);
  {
    size_t const cSize =
        frame ? ZSTD_compress_frameChunk(cctx, dst, dstCapacity, src, srcSize,
                                         lastFrameChunk)
              : ZSTD_compressBlock_internal(cctx, dst, dstCapacity, src,
                                            srcSize, 0);
    FORWARD_IF_ERROR(cSize, "%s",
                     frame ? "ZSTD_compress_frameChunk failed"
                           : "ZSTD_compressBlock_internal failed");
    cctx->consumedSrcSize += srcSize;
    cctx->producedCSize += (cSize + fhSize);
    assert(!(cctx->appliedParams.fParams.contentSizeFlag &&
             cctx->pledgedSrcSizePlusOne == 0));
    if (cctx->pledgedSrcSizePlusOne != 0) {
      ZSTD_STATIC_ASSERT(ZSTD_CONTENTSIZE_UNKNOWN == (unsigned long long)-1);
      RETURN_ERROR_IF(cctx->consumedSrcSize + 1 > cctx->pledgedSrcSizePlusOne,
                      srcSize_wrong,
                      "error : pledgedSrcSize = %u, while realSrcSize >= %u",
                      (unsigned)cctx->pledgedSrcSizePlusOne - 1,
                      (unsigned)cctx->consumedSrcSize);
    }
    return cSize + fhSize;
  }
}

size_t ZSTD_compressContinue_public(ZSTD_CCtx *cctx, void *dst,
                                    size_t dstCapacity, const void *src,
                                    size_t srcSize) {
  DEBUGLOG(5, "ZSTD_compressContinue (srcSize=%u)", (unsigned)srcSize);
  return ZSTD_compressContinue_internal(cctx, dst, dstCapacity, src, srcSize, 1,
                                        0);
}

size_t ZSTD_compressContinue(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                             const void *src, size_t srcSize) {
  return ZSTD_compressContinue_public(cctx, dst, dstCapacity, src, srcSize);
}

static size_t ZSTD_getBlockSize_deprecated(const ZSTD_CCtx *cctx) {
  ZSTD_compressionParameters const cParams = cctx->appliedParams.cParams;
  assert(!ZSTD_checkCParams(cParams));
  return MIN(cctx->appliedParams.maxBlockSize, (size_t)1 << cParams.windowLog);
}

size_t ZSTD_getBlockSize(const ZSTD_CCtx *cctx) {
  return ZSTD_getBlockSize_deprecated(cctx);
}

size_t ZSTD_compressBlock_deprecated(ZSTD_CCtx *cctx, void *dst,
                                     size_t dstCapacity, const void *src,
                                     size_t srcSize) {
  DEBUGLOG(5, "ZSTD_compressBlock: srcSize = %u", (unsigned)srcSize);
  {
    size_t const blockSizeMax = ZSTD_getBlockSize_deprecated(cctx);
    RETURN_ERROR_IF(srcSize > blockSizeMax, srcSize_wrong,
                    "input is larger than a block");
  }

  return ZSTD_compressContinue_internal(cctx, dst, dstCapacity, src, srcSize, 0,
                                        0);
}

size_t ZSTD_compressBlock(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                          const void *src, size_t srcSize) {
  return ZSTD_compressBlock_deprecated(cctx, dst, dstCapacity, src, srcSize);
}

static size_t ZSTD_loadDictionaryContent(ZSTD_MatchState_t *ms, ldmState_t *ls,
                                         ZSTD_cwksp *ws,
                                         ZSTD_CCtx_params const *params,
                                         const void *src, size_t srcSize,
                                         ZSTD_dictTableLoadMethod_e dtlm,
                                         ZSTD_tableFillPurpose_e tfp) {
  const BYTE *ip = (const BYTE *)src;
  const BYTE *const iend = ip + srcSize;
  int const loadLdmDict =
      params->ldmParams.enableLdm == ZSTD_ps_enable && ls != NULL;

  ZSTD_assertEqualCParams(params->cParams, ms->cParams);

  {

    U32 maxDictSize = ZSTD_CURRENT_MAX - ZSTD_WINDOW_START_INDEX;

    int const CDictTaggedIndices = ZSTD_CDictIndicesAreTagged(&params->cParams);
    if (CDictTaggedIndices && tfp == ZSTD_tfp_forCDict) {

      U32 const shortCacheMaxDictSize =
          (1u << (32 - ZSTD_SHORT_CACHE_TAG_BITS)) - ZSTD_WINDOW_START_INDEX;
      maxDictSize = MIN(maxDictSize, shortCacheMaxDictSize);
      assert(!loadLdmDict);
    }

    if (srcSize > maxDictSize) {
      ip = iend - maxDictSize;
      src = ip;
      srcSize = maxDictSize;
    }
  }

  if (srcSize > ZSTD_CHUNKSIZE_MAX) {

    assert(ZSTD_window_isEmpty(ms->window));
    if (loadLdmDict)
      assert(ZSTD_window_isEmpty(ls->window));
  }
  ZSTD_window_update(&ms->window, src, srcSize, 0);

  DEBUGLOG(4, "ZSTD_loadDictionaryContent: useRowMatchFinder=%d",
           (int)params->useRowMatchFinder);

  if (loadLdmDict) {
    DEBUGLOG(4, "ZSTD_loadDictionaryContent: Trigger loadLdmDict");
    ZSTD_window_update(&ls->window, src, srcSize, 0);
    ls->loadedDictEnd = params->forceWindow ? 0 : (U32)(iend - ls->window.base);
    ZSTD_ldm_fillHashTable(ls, ip, iend, &params->ldmParams);
    DEBUGLOG(4, "ZSTD_loadDictionaryContent: ZSTD_ldm_fillHashTable completes");
  }

  {
    U32 maxDictSize =
        1U << MIN(
            MAX(params->cParams.hashLog + 3, params->cParams.chainLog + 1), 31);
    if (srcSize > maxDictSize) {
      ip = iend - maxDictSize;
      src = ip;
      srcSize = maxDictSize;
    }
  }

  ms->nextToUpdate = (U32)(ip - ms->window.base);
  ms->loadedDictEnd = params->forceWindow ? 0 : (U32)(iend - ms->window.base);
  ms->forceNonContiguous = params->deterministicRefPrefix;

  if (srcSize <= HASH_READ_SIZE)
    return 0;

  ZSTD_overflowCorrectIfNeeded(ms, ws, params, ip, iend);

  switch (params->cParams.strategy) {
  case ZSTD_fast:
    ZSTD_fillHashTable(ms, iend, dtlm, tfp);
    break;
  case ZSTD_dfast:
#ifndef ZSTD_EXCLUDE_DFAST_BLOCK_COMPRESSOR
    ZSTD_fillDoubleHashTable(ms, iend, dtlm, tfp);
#else
    assert(0);
#endif
    break;

  case ZSTD_greedy:
  case ZSTD_lazy:
  case ZSTD_lazy2:
#if !defined(ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR) ||                          \
    !defined(ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR) ||                            \
    !defined(ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR)
    assert(srcSize >= HASH_READ_SIZE);
    if (ms->dedicatedDictSearch) {
      assert(ms->chainTable != NULL);
      ZSTD_dedicatedDictSearch_lazy_loadDictionary(ms, iend - HASH_READ_SIZE);
    } else {
      assert(params->useRowMatchFinder != ZSTD_ps_auto);
      if (params->useRowMatchFinder == ZSTD_ps_enable) {
        size_t const tagTableSize = ((size_t)1 << params->cParams.hashLog);
        ZSTD_memset(ms->tagTable, 0, tagTableSize);
        ZSTD_row_update(ms, iend - HASH_READ_SIZE);
        DEBUGLOG(4, "Using row-based hash table for lazy dict");
      } else {
        ZSTD_insertAndFindFirstIndex(ms, iend - HASH_READ_SIZE);
        DEBUGLOG(4, "Using chain-based hash table for lazy dict");
      }
    }
#else
    assert(0);
#endif
    break;

  case ZSTD_btlazy2:
  case ZSTD_btopt:
  case ZSTD_btultra:
  case ZSTD_btultra2:
#if !defined(ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR) ||                         \
    !defined(ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR) ||                           \
    !defined(ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR)
    assert(srcSize >= HASH_READ_SIZE);
    DEBUGLOG(4, "Fill %u bytes into the Binary Tree", (unsigned)srcSize);
    ZSTD_updateTree(ms, iend - HASH_READ_SIZE, iend);
#else
    assert(0);
#endif
    break;

  default:
    assert(0);
  }

  ms->nextToUpdate = (U32)(iend - ms->window.base);
  return 0;
}

static FSE_repeat ZSTD_dictNCountRepeat(short *normalizedCounter,
                                        unsigned dictMaxSymbolValue,
                                        unsigned maxSymbolValue) {
  U32 s;
  if (dictMaxSymbolValue < maxSymbolValue) {
    return FSE_repeat_check;
  }
  for (s = 0; s <= maxSymbolValue; ++s) {
    if (normalizedCounter[s] == 0) {
      return FSE_repeat_check;
    }
  }
  return FSE_repeat_valid;
}

size_t ZSTD_loadCEntropy(ZSTD_compressedBlockState_t *bs, void *workspace,
                         const void *const dict, size_t dictSize) {
  short offcodeNCount[MaxOff + 1];
  unsigned offcodeMaxValue = MaxOff;
  const BYTE *dictPtr = (const BYTE *)dict;
  const BYTE *const dictEnd = dictPtr + dictSize;
  dictPtr += 8;
  bs->entropy.huf.repeatMode = HUF_repeat_check;

  {
    unsigned maxSymbolValue = 255;
    unsigned hasZeroWeights = 1;
    size_t const hufHeaderSize =
        HUF_readCTable((HUF_CElt *)bs->entropy.huf.CTable, &maxSymbolValue,
                       dictPtr, (size_t)(dictEnd - dictPtr), &hasZeroWeights);

    if (!hasZeroWeights && maxSymbolValue == 255)
      bs->entropy.huf.repeatMode = HUF_repeat_valid;

    RETURN_ERROR_IF(HUF_isError(hufHeaderSize), dictionary_corrupted, "");
    dictPtr += hufHeaderSize;
  }

  {
    unsigned offcodeLog;
    size_t const offcodeHeaderSize =
        FSE_readNCount(offcodeNCount, &offcodeMaxValue, &offcodeLog, dictPtr,
                       (size_t)(dictEnd - dictPtr));
    RETURN_ERROR_IF(FSE_isError(offcodeHeaderSize), dictionary_corrupted, "");
    RETURN_ERROR_IF(offcodeLog > OffFSELog, dictionary_corrupted, "");

    RETURN_ERROR_IF(FSE_isError(FSE_buildCTable_wksp(
                        bs->entropy.fse.offcodeCTable, offcodeNCount, MaxOff,
                        offcodeLog, workspace, HUF_WORKSPACE_SIZE)),
                    dictionary_corrupted, "");

    dictPtr += offcodeHeaderSize;
  }

  {
    short matchlengthNCount[MaxML + 1];
    unsigned matchlengthMaxValue = MaxML, matchlengthLog;
    size_t const matchlengthHeaderSize =
        FSE_readNCount(matchlengthNCount, &matchlengthMaxValue, &matchlengthLog,
                       dictPtr, (size_t)(dictEnd - dictPtr));
    RETURN_ERROR_IF(FSE_isError(matchlengthHeaderSize), dictionary_corrupted,
                    "");
    RETURN_ERROR_IF(matchlengthLog > MLFSELog, dictionary_corrupted, "");
    RETURN_ERROR_IF(FSE_isError(FSE_buildCTable_wksp(
                        bs->entropy.fse.matchlengthCTable, matchlengthNCount,
                        matchlengthMaxValue, matchlengthLog, workspace,
                        HUF_WORKSPACE_SIZE)),
                    dictionary_corrupted, "");
    bs->entropy.fse.matchlength_repeatMode =
        ZSTD_dictNCountRepeat(matchlengthNCount, matchlengthMaxValue, MaxML);
    dictPtr += matchlengthHeaderSize;
  }

  {
    short litlengthNCount[MaxLL + 1];
    unsigned litlengthMaxValue = MaxLL, litlengthLog;
    size_t const litlengthHeaderSize =
        FSE_readNCount(litlengthNCount, &litlengthMaxValue, &litlengthLog,
                       dictPtr, (size_t)(dictEnd - dictPtr));
    RETURN_ERROR_IF(FSE_isError(litlengthHeaderSize), dictionary_corrupted, "");
    RETURN_ERROR_IF(litlengthLog > LLFSELog, dictionary_corrupted, "");
    RETURN_ERROR_IF(
        FSE_isError(FSE_buildCTable_wksp(
            bs->entropy.fse.litlengthCTable, litlengthNCount, litlengthMaxValue,
            litlengthLog, workspace, HUF_WORKSPACE_SIZE)),
        dictionary_corrupted, "");
    bs->entropy.fse.litlength_repeatMode =
        ZSTD_dictNCountRepeat(litlengthNCount, litlengthMaxValue, MaxLL);
    dictPtr += litlengthHeaderSize;
  }

  RETURN_ERROR_IF(dictPtr + 12 > dictEnd, dictionary_corrupted, "");
  bs->rep[0] = MEM_readLE32(dictPtr + 0);
  bs->rep[1] = MEM_readLE32(dictPtr + 4);
  bs->rep[2] = MEM_readLE32(dictPtr + 8);
  dictPtr += 12;

  {
    size_t const dictContentSize = (size_t)(dictEnd - dictPtr);
    U32 offcodeMax = MaxOff;
    if (dictContentSize <= ((U32)-1) - 128 KB) {
      U32 const maxOffset = (U32)dictContentSize + 128 KB;
      offcodeMax = ZSTD_highbit32(maxOffset);
    }

    bs->entropy.fse.offcode_repeatMode = ZSTD_dictNCountRepeat(
        offcodeNCount, offcodeMaxValue, MIN(offcodeMax, MaxOff));

    {
      U32 u;
      for (u = 0; u < 3; u++) {
        RETURN_ERROR_IF(bs->rep[u] == 0, dictionary_corrupted, "");
        RETURN_ERROR_IF(bs->rep[u] > dictContentSize, dictionary_corrupted, "");
      }
    }
  }

  return (size_t)(dictPtr - (const BYTE *)dict);
}

static size_t ZSTD_loadZstdDictionary(ZSTD_compressedBlockState_t *bs,
                                      ZSTD_MatchState_t *ms, ZSTD_cwksp *ws,
                                      ZSTD_CCtx_params const *params,
                                      const void *dict, size_t dictSize,
                                      ZSTD_dictTableLoadMethod_e dtlm,
                                      ZSTD_tableFillPurpose_e tfp,
                                      void *workspace) {
  const BYTE *dictPtr = (const BYTE *)dict;
  const BYTE *const dictEnd = dictPtr + dictSize;
  size_t dictID;
  size_t eSize;
  ZSTD_STATIC_ASSERT(HUF_WORKSPACE_SIZE >= (1 << MAX(MLFSELog, LLFSELog)));
  assert(dictSize >= 8);
  assert(MEM_readLE32(dictPtr) == ZSTD_MAGIC_DICTIONARY);

  dictID = params->fParams.noDictIDFlag ? 0 : MEM_readLE32(dictPtr + 4);
  eSize = ZSTD_loadCEntropy(bs, workspace, dict, dictSize);
  FORWARD_IF_ERROR(eSize, "ZSTD_loadCEntropy failed");
  dictPtr += eSize;

  {
    size_t const dictContentSize = (size_t)(dictEnd - dictPtr);
    FORWARD_IF_ERROR(ZSTD_loadDictionaryContent(ms, NULL, ws, params, dictPtr,
                                                dictContentSize, dtlm, tfp),
                     "");
  }
  return dictID;
}

static size_t ZSTD_compress_insertDictionary(
    ZSTD_compressedBlockState_t *bs, ZSTD_MatchState_t *ms, ldmState_t *ls,
    ZSTD_cwksp *ws, const ZSTD_CCtx_params *params, const void *dict,
    size_t dictSize, ZSTD_dictContentType_e dictContentType,
    ZSTD_dictTableLoadMethod_e dtlm, ZSTD_tableFillPurpose_e tfp,
    void *workspace) {
  DEBUGLOG(4, "ZSTD_compress_insertDictionary (dictSize=%u)", (U32)dictSize);
  if ((dict == NULL) || (dictSize < 8)) {
    RETURN_ERROR_IF(dictContentType == ZSTD_dct_fullDict, dictionary_wrong, "");
    return 0;
  }

  ZSTD_reset_compressedBlockState(bs);

  if (dictContentType == ZSTD_dct_rawContent)
    return ZSTD_loadDictionaryContent(ms, ls, ws, params, dict, dictSize, dtlm,
                                      tfp);

  if (MEM_readLE32(dict) != ZSTD_MAGIC_DICTIONARY) {
    if (dictContentType == ZSTD_dct_auto) {
      DEBUGLOG(4, "raw content dictionary detected");
      return ZSTD_loadDictionaryContent(ms, ls, ws, params, dict, dictSize,
                                        dtlm, tfp);
    }
    RETURN_ERROR_IF(dictContentType == ZSTD_dct_fullDict, dictionary_wrong, "");
    assert(0);
  }

  return ZSTD_loadZstdDictionary(bs, ms, ws, params, dict, dictSize, dtlm, tfp,
                                 workspace);
}

#define ZSTD_USE_CDICT_PARAMS_SRCSIZE_CUTOFF (128 KB)
#define ZSTD_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER (6ULL)

static size_t ZSTD_compressBegin_internal(
    ZSTD_CCtx *cctx, const void *dict, size_t dictSize,
    ZSTD_dictContentType_e dictContentType, ZSTD_dictTableLoadMethod_e dtlm,
    const ZSTD_CDict *cdict, const ZSTD_CCtx_params *params, U64 pledgedSrcSize,
    ZSTD_buffered_policy_e zbuff) {
  size_t const dictContentSize = cdict ? cdict->dictContentSize : dictSize;
#if ZSTD_TRACE
  cctx->traceCtx =
      (ZSTD_trace_compress_begin != NULL) ? ZSTD_trace_compress_begin(cctx) : 0;
#endif
  DEBUGLOG(4, "ZSTD_compressBegin_internal: wlog=%u",
           params->cParams.windowLog);

  assert(!ZSTD_isError(ZSTD_checkCParams(params->cParams)));
  assert(!((dict) && (cdict)));
  if ((cdict) && (cdict->dictContentSize > 0) &&
      (pledgedSrcSize < ZSTD_USE_CDICT_PARAMS_SRCSIZE_CUTOFF ||
       pledgedSrcSize <
           cdict->dictContentSize * ZSTD_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER ||
       pledgedSrcSize == ZSTD_CONTENTSIZE_UNKNOWN ||
       cdict->compressionLevel == 0) &&
      (params->attachDictPref != ZSTD_dictForceLoad)) {
    return ZSTD_resetCCtx_usingCDict(cctx, cdict, params, pledgedSrcSize,
                                     zbuff);
  }

  FORWARD_IF_ERROR(ZSTD_resetCCtx_internal(cctx, params, pledgedSrcSize,
                                           dictContentSize, ZSTDcrp_makeClean,
                                           zbuff),
                   "");
  {
    size_t const dictID =
        cdict ? ZSTD_compress_insertDictionary(
                    cctx->blockState.prevCBlock, &cctx->blockState.matchState,
                    &cctx->ldmState, &cctx->workspace, &cctx->appliedParams,
                    cdict->dictContent, cdict->dictContentSize,
                    cdict->dictContentType, dtlm, ZSTD_tfp_forCCtx,
                    cctx->tmpWorkspace)
              : ZSTD_compress_insertDictionary(
                    cctx->blockState.prevCBlock, &cctx->blockState.matchState,
                    &cctx->ldmState, &cctx->workspace, &cctx->appliedParams,
                    dict, dictSize, dictContentType, dtlm, ZSTD_tfp_forCCtx,
                    cctx->tmpWorkspace);
    FORWARD_IF_ERROR(dictID, "ZSTD_compress_insertDictionary failed");
    assert(dictID <= UINT_MAX);
    cctx->dictID = (U32)dictID;
    cctx->dictContentSize = dictContentSize;
  }
  return 0;
}

size_t ZSTD_compressBegin_advanced_internal(
    ZSTD_CCtx *cctx, const void *dict, size_t dictSize,
    ZSTD_dictContentType_e dictContentType, ZSTD_dictTableLoadMethod_e dtlm,
    const ZSTD_CDict *cdict, const ZSTD_CCtx_params *params,
    unsigned long long pledgedSrcSize) {
  DEBUGLOG(4, "ZSTD_compressBegin_advanced_internal: wlog=%u",
           params->cParams.windowLog);

  FORWARD_IF_ERROR(ZSTD_checkCParams(params->cParams), "");
  return ZSTD_compressBegin_internal(cctx, dict, dictSize, dictContentType,
                                     dtlm, cdict, params, pledgedSrcSize,
                                     ZSTDb_not_buffered);
}

size_t ZSTD_compressBegin_advanced(ZSTD_CCtx *cctx, const void *dict,
                                   size_t dictSize, ZSTD_parameters params,
                                   unsigned long long pledgedSrcSize) {
  ZSTD_CCtx_params cctxParams;
  ZSTD_CCtxParams_init_internal(&cctxParams, &params, ZSTD_NO_CLEVEL);
  return ZSTD_compressBegin_advanced_internal(
      cctx, dict, dictSize, ZSTD_dct_auto, ZSTD_dtlm_fast, NULL, &cctxParams,
      pledgedSrcSize);
}

static size_t ZSTD_compressBegin_usingDict_deprecated(ZSTD_CCtx *cctx,
                                                      const void *dict,
                                                      size_t dictSize,
                                                      int compressionLevel) {
  ZSTD_CCtx_params cctxParams;
  {
    ZSTD_parameters const params =
        ZSTD_getParams_internal(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN,
                                dictSize, ZSTD_cpm_noAttachDict);
    ZSTD_CCtxParams_init_internal(&cctxParams, &params,
                                  (compressionLevel == 0) ? ZSTD_CLEVEL_DEFAULT
                                                          : compressionLevel);
  }
  DEBUGLOG(4, "ZSTD_compressBegin_usingDict (dictSize=%u)", (unsigned)dictSize);
  return ZSTD_compressBegin_internal(
      cctx, dict, dictSize, ZSTD_dct_auto, ZSTD_dtlm_fast, NULL, &cctxParams,
      ZSTD_CONTENTSIZE_UNKNOWN, ZSTDb_not_buffered);
}

size_t ZSTD_compressBegin_usingDict(ZSTD_CCtx *cctx, const void *dict,
                                    size_t dictSize, int compressionLevel) {
  return ZSTD_compressBegin_usingDict_deprecated(cctx, dict, dictSize,
                                                 compressionLevel);
}

size_t ZSTD_compressBegin(ZSTD_CCtx *cctx, int compressionLevel) {
  return ZSTD_compressBegin_usingDict_deprecated(cctx, NULL, 0,
                                                 compressionLevel);
}

static size_t ZSTD_writeEpilogue(ZSTD_CCtx *cctx, void *dst,
                                 size_t dstCapacity) {
  BYTE *const ostart = (BYTE *)dst;
  BYTE *op = ostart;

  DEBUGLOG(4, "ZSTD_writeEpilogue");
  RETURN_ERROR_IF(cctx->stage == ZSTDcs_created, stage_wrong, "init missing");

  if (cctx->stage == ZSTDcs_init) {
    size_t fhSize =
        ZSTD_writeFrameHeader(dst, dstCapacity, &cctx->appliedParams, 0, 0);
    FORWARD_IF_ERROR(fhSize, "ZSTD_writeFrameHeader failed");
    dstCapacity -= fhSize;
    op += fhSize;
    cctx->stage = ZSTDcs_ongoing;
  }

  if (cctx->stage != ZSTDcs_ending) {

    U32 const cBlockHeader24 = 1 + (((U32)bt_raw) << 1) + 0;
    ZSTD_STATIC_ASSERT(ZSTD_BLOCKHEADERSIZE == 3);
    RETURN_ERROR_IF(dstCapacity < 3, dstSize_tooSmall, "no room for epilogue");
    MEM_writeLE24(op, cBlockHeader24);
    op += ZSTD_blockHeaderSize;
    dstCapacity -= ZSTD_blockHeaderSize;
  }

  if (cctx->appliedParams.fParams.checksumFlag) {
    U32 const checksum = (U32)XXH64_digest(&cctx->xxhState);
    RETURN_ERROR_IF(dstCapacity < 4, dstSize_tooSmall, "no room for checksum");
    DEBUGLOG(4, "ZSTD_writeEpilogue: write checksum : %08X",
             (unsigned)checksum);
    MEM_writeLE32(op, checksum);
    op += 4;
  }

  cctx->stage = ZSTDcs_created;
  return (size_t)(op - ostart);
}

void ZSTD_CCtx_trace(ZSTD_CCtx *cctx, size_t extraCSize) {
#if ZSTD_TRACE
  if (cctx->traceCtx && ZSTD_trace_compress_end != NULL) {
    int const streaming = cctx->inBuffSize > 0 || cctx->outBuffSize > 0 ||
                          cctx->appliedParams.nbWorkers > 0;
    ZSTD_Trace trace;
    ZSTD_memset(&trace, 0, sizeof(trace));
    trace.version = ZSTD_VERSION_NUMBER;
    trace.streaming = streaming;
    trace.dictionaryID = cctx->dictID;
    trace.dictionarySize = cctx->dictContentSize;
    trace.uncompressedSize = cctx->consumedSrcSize;
    trace.compressedSize = cctx->producedCSize + extraCSize;
    trace.params = &cctx->appliedParams;
    trace.cctx = cctx;
    ZSTD_trace_compress_end(cctx->traceCtx, &trace);
  }
  cctx->traceCtx = 0;
#else
  (void)cctx;
  (void)extraCSize;
#endif
}

size_t ZSTD_compressEnd_public(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                               const void *src, size_t srcSize) {
  size_t endResult;
  size_t const cSize = ZSTD_compressContinue_internal(cctx, dst, dstCapacity,
                                                      src, srcSize, 1, 1);
  FORWARD_IF_ERROR(cSize, "ZSTD_compressContinue_internal failed");
  endResult =
      ZSTD_writeEpilogue(cctx, (char *)dst + cSize, dstCapacity - cSize);
  FORWARD_IF_ERROR(endResult, "ZSTD_writeEpilogue failed");
  assert(!(cctx->appliedParams.fParams.contentSizeFlag &&
           cctx->pledgedSrcSizePlusOne == 0));
  if (cctx->pledgedSrcSizePlusOne != 0) {
    ZSTD_STATIC_ASSERT(ZSTD_CONTENTSIZE_UNKNOWN == (unsigned long long)-1);
    DEBUGLOG(4, "end of frame : controlling src size");
    RETURN_ERROR_IF(cctx->pledgedSrcSizePlusOne != cctx->consumedSrcSize + 1,
                    srcSize_wrong,
                    "error : pledgedSrcSize = %u, while realSrcSize = %u",
                    (unsigned)cctx->pledgedSrcSizePlusOne - 1,
                    (unsigned)cctx->consumedSrcSize);
  }
  ZSTD_CCtx_trace(cctx, endResult);
  return cSize + endResult;
}

size_t ZSTD_compressEnd(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                        const void *src, size_t srcSize) {
  return ZSTD_compressEnd_public(cctx, dst, dstCapacity, src, srcSize);
}

size_t ZSTD_compress_advanced(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                              const void *src, size_t srcSize, const void *dict,
                              size_t dictSize, ZSTD_parameters params) {
  DEBUGLOG(4, "ZSTD_compress_advanced");
  FORWARD_IF_ERROR(ZSTD_checkCParams(params.cParams), "");
  ZSTD_CCtxParams_init_internal(&cctx->simpleApiParams, &params,
                                ZSTD_NO_CLEVEL);
  return ZSTD_compress_advanced_internal(cctx, dst, dstCapacity, src, srcSize,
                                         dict, dictSize,
                                         &cctx->simpleApiParams);
}

size_t ZSTD_compress_advanced_internal(ZSTD_CCtx *cctx, void *dst,
                                       size_t dstCapacity, const void *src,
                                       size_t srcSize, const void *dict,
                                       size_t dictSize,
                                       const ZSTD_CCtx_params *params) {
  DEBUGLOG(4, "ZSTD_compress_advanced_internal (srcSize:%u)",
           (unsigned)srcSize);
  FORWARD_IF_ERROR(ZSTD_compressBegin_internal(
                       cctx, dict, dictSize, ZSTD_dct_auto, ZSTD_dtlm_fast,
                       NULL, params, srcSize, ZSTDb_not_buffered),
                   "");
  return ZSTD_compressEnd_public(cctx, dst, dstCapacity, src, srcSize);
}

size_t ZSTD_compress_usingDict(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                               const void *src, size_t srcSize,
                               const void *dict, size_t dictSize,
                               int compressionLevel) {
  {
    ZSTD_parameters const params = ZSTD_getParams_internal(
        compressionLevel, srcSize, dict ? dictSize : 0, ZSTD_cpm_noAttachDict);
    assert(params.fParams.contentSizeFlag == 1);
    ZSTD_CCtxParams_init_internal(&cctx->simpleApiParams, &params,
                                  (compressionLevel == 0) ? ZSTD_CLEVEL_DEFAULT
                                                          : compressionLevel);
  }
  DEBUGLOG(4, "ZSTD_compress_usingDict (srcSize=%u)", (unsigned)srcSize);
  return ZSTD_compress_advanced_internal(cctx, dst, dstCapacity, src, srcSize,
                                         dict, dictSize,
                                         &cctx->simpleApiParams);
}

size_t ZSTD_compressCCtx(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                         const void *src, size_t srcSize,
                         int compressionLevel) {
  DEBUGLOG(4, "ZSTD_compressCCtx (srcSize=%u)", (unsigned)srcSize);
  assert(cctx != NULL);
  return ZSTD_compress_usingDict(cctx, dst, dstCapacity, src, srcSize, NULL, 0,
                                 compressionLevel);
}

size_t ZSTD_compress(void *dst, size_t dstCapacity, const void *src,
                     size_t srcSize, int compressionLevel) {
  size_t result;
#if ZSTD_COMPRESS_HEAPMODE
  ZSTD_CCtx *cctx = ZSTD_createCCtx();
  RETURN_ERROR_IF(!cctx, memory_allocation, "ZSTD_createCCtx failed");
  result =
      ZSTD_compressCCtx(cctx, dst, dstCapacity, src, srcSize, compressionLevel);
  ZSTD_freeCCtx(cctx);
#else
  ZSTD_CCtx ctxBody;
  ZSTD_initCCtx(&ctxBody, ZSTD_defaultCMem);
  result = ZSTD_compressCCtx(&ctxBody, dst, dstCapacity, src, srcSize,
                             compressionLevel);
  ZSTD_freeCCtxContent(&ctxBody);

#endif
  return result;
}

size_t ZSTD_estimateCDictSize_advanced(size_t dictSize,
                                       ZSTD_compressionParameters cParams,
                                       ZSTD_dictLoadMethod_e dictLoadMethod) {
  DEBUGLOG(5, "sizeof(ZSTD_CDict) : %u", (unsigned)sizeof(ZSTD_CDict));
  return ZSTD_cwksp_alloc_size(sizeof(ZSTD_CDict)) +
         ZSTD_cwksp_alloc_size(HUF_WORKSPACE_SIZE)

         + ZSTD_sizeof_matchState(
               &cParams, ZSTD_resolveRowMatchFinderMode(ZSTD_ps_auto, &cParams),
               1, 0) +
         (dictLoadMethod == ZSTD_dlm_byRef
              ? 0
              : ZSTD_cwksp_alloc_size(
                    ZSTD_cwksp_align(dictSize, sizeof(void *))));
}

size_t ZSTD_estimateCDictSize(size_t dictSize, int compressionLevel) {
  ZSTD_compressionParameters const cParams =
      ZSTD_getCParams_internal(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN,
                               dictSize, ZSTD_cpm_createCDict);
  return ZSTD_estimateCDictSize_advanced(dictSize, cParams, ZSTD_dlm_byCopy);
}

size_t ZSTD_sizeof_CDict(const ZSTD_CDict *cdict) {
  if (cdict == NULL)
    return 0;
  DEBUGLOG(5, "sizeof(*cdict) : %u", (unsigned)sizeof(*cdict));

  return (cdict->workspace.workspace == cdict ? 0 : sizeof(*cdict)) +
         ZSTD_cwksp_sizeof(&cdict->workspace);
}

static size_t ZSTD_initCDict_internal(ZSTD_CDict *cdict, const void *dictBuffer,
                                      size_t dictSize,
                                      ZSTD_dictLoadMethod_e dictLoadMethod,
                                      ZSTD_dictContentType_e dictContentType,
                                      ZSTD_CCtx_params params) {
  DEBUGLOG(3, "ZSTD_initCDict_internal (dictContentType:%u)",
           (unsigned)dictContentType);
  assert(!ZSTD_checkCParams(params.cParams));
  cdict->matchState.cParams = params.cParams;
  cdict->matchState.dedicatedDictSearch = params.enableDedicatedDictSearch;
  if ((dictLoadMethod == ZSTD_dlm_byRef) || (!dictBuffer) || (!dictSize)) {
    cdict->dictContent = dictBuffer;
  } else {
    void *internalBuffer = ZSTD_cwksp_reserve_object(
        &cdict->workspace, ZSTD_cwksp_align(dictSize, sizeof(void *)));
    RETURN_ERROR_IF(!internalBuffer, memory_allocation, "NULL pointer!");
    cdict->dictContent = internalBuffer;
    ZSTD_memcpy(internalBuffer, dictBuffer, dictSize);
  }
  cdict->dictContentSize = dictSize;
  cdict->dictContentType = dictContentType;

  cdict->entropyWorkspace =
      (U32 *)ZSTD_cwksp_reserve_object(&cdict->workspace, HUF_WORKSPACE_SIZE);

  ZSTD_reset_compressedBlockState(&cdict->cBlockState);
  FORWARD_IF_ERROR(ZSTD_reset_matchState(
                       &cdict->matchState, &cdict->workspace, &params.cParams,
                       params.useRowMatchFinder, ZSTDcrp_makeClean,
                       ZSTDirp_reset, ZSTD_resetTarget_CDict),
                   "");

  {
    params.compressionLevel = ZSTD_CLEVEL_DEFAULT;
    params.fParams.contentSizeFlag = 1;
    {
      size_t const dictID = ZSTD_compress_insertDictionary(
          &cdict->cBlockState, &cdict->matchState, NULL, &cdict->workspace,
          &params, cdict->dictContent, cdict->dictContentSize, dictContentType,
          ZSTD_dtlm_full, ZSTD_tfp_forCDict, cdict->entropyWorkspace);
      FORWARD_IF_ERROR(dictID, "ZSTD_compress_insertDictionary failed");
      assert(dictID <= (size_t)(U32)-1);
      cdict->dictID = (U32)dictID;
    }
  }

  return 0;
}

static ZSTD_CDict *ZSTD_createCDict_advanced_internal(
    size_t dictSize, ZSTD_dictLoadMethod_e dictLoadMethod,
    ZSTD_compressionParameters cParams, ZSTD_ParamSwitch_e useRowMatchFinder,
    int enableDedicatedDictSearch, ZSTD_customMem customMem) {
  if ((!customMem.customAlloc) ^ (!customMem.customFree))
    return NULL;
  DEBUGLOG(3, "ZSTD_createCDict_advanced_internal (dictSize=%u)",
           (unsigned)dictSize);

  {
    size_t const workspaceSize =
        ZSTD_cwksp_alloc_size(sizeof(ZSTD_CDict)) +
        ZSTD_cwksp_alloc_size(HUF_WORKSPACE_SIZE) +
        ZSTD_sizeof_matchState(&cParams, useRowMatchFinder,
                               enableDedicatedDictSearch, 0) +
        (dictLoadMethod == ZSTD_dlm_byRef
             ? 0
             : ZSTD_cwksp_alloc_size(
                   ZSTD_cwksp_align(dictSize, sizeof(void *))));
    void *const workspace = ZSTD_customMalloc(workspaceSize, customMem);
    ZSTD_cwksp ws;
    ZSTD_CDict *cdict;

    if (!workspace) {
      ZSTD_customFree(workspace, customMem);
      return NULL;
    }

    ZSTD_cwksp_init(&ws, workspace, workspaceSize, ZSTD_cwksp_dynamic_alloc);

    cdict = (ZSTD_CDict *)ZSTD_cwksp_reserve_object(&ws, sizeof(ZSTD_CDict));
    assert(cdict != NULL);
    ZSTD_cwksp_move(&cdict->workspace, &ws);
    cdict->customMem = customMem;
    cdict->compressionLevel = ZSTD_NO_CLEVEL;
    cdict->useRowMatchFinder = useRowMatchFinder;
    return cdict;
  }
}

ZSTD_CDict *ZSTD_createCDict_advanced(const void *dictBuffer, size_t dictSize,
                                      ZSTD_dictLoadMethod_e dictLoadMethod,
                                      ZSTD_dictContentType_e dictContentType,
                                      ZSTD_compressionParameters cParams,
                                      ZSTD_customMem customMem) {
  ZSTD_CCtx_params cctxParams;
  ZSTD_memset(&cctxParams, 0, sizeof(cctxParams));
  DEBUGLOG(3, "ZSTD_createCDict_advanced, dictSize=%u, mode=%u",
           (unsigned)dictSize, (unsigned)dictContentType);
  ZSTD_CCtxParams_init(&cctxParams, 0);
  cctxParams.cParams = cParams;
  cctxParams.customMem = customMem;
  return ZSTD_createCDict_advanced2(dictBuffer, dictSize, dictLoadMethod,
                                    dictContentType, &cctxParams, customMem);
}

ZSTD_CDict *ZSTD_createCDict_advanced2(
    const void *dict, size_t dictSize, ZSTD_dictLoadMethod_e dictLoadMethod,
    ZSTD_dictContentType_e dictContentType,
    const ZSTD_CCtx_params *originalCctxParams, ZSTD_customMem customMem) {
  ZSTD_CCtx_params cctxParams = *originalCctxParams;
  ZSTD_compressionParameters cParams;
  ZSTD_CDict *cdict;

  DEBUGLOG(3, "ZSTD_createCDict_advanced2, dictSize=%u, mode=%u",
           (unsigned)dictSize, (unsigned)dictContentType);
  if (!customMem.customAlloc ^ !customMem.customFree)
    return NULL;

  if (cctxParams.enableDedicatedDictSearch) {
    cParams = ZSTD_dedicatedDictSearch_getCParams(cctxParams.compressionLevel,
                                                  dictSize);
    ZSTD_overrideCParams(&cParams, &cctxParams.cParams);
  } else {
    cParams = ZSTD_getCParamsFromCCtxParams(
        &cctxParams, ZSTD_CONTENTSIZE_UNKNOWN, dictSize, ZSTD_cpm_createCDict);
  }

  if (!ZSTD_dedicatedDictSearch_isSupported(&cParams)) {

    cctxParams.enableDedicatedDictSearch = 0;
    cParams = ZSTD_getCParamsFromCCtxParams(
        &cctxParams, ZSTD_CONTENTSIZE_UNKNOWN, dictSize, ZSTD_cpm_createCDict);
  }

  DEBUGLOG(3, "ZSTD_createCDict_advanced2: DedicatedDictSearch=%u",
           cctxParams.enableDedicatedDictSearch);
  cctxParams.cParams = cParams;
  cctxParams.useRowMatchFinder =
      ZSTD_resolveRowMatchFinderMode(cctxParams.useRowMatchFinder, &cParams);

  cdict = ZSTD_createCDict_advanced_internal(
      dictSize, dictLoadMethod, cctxParams.cParams,
      cctxParams.useRowMatchFinder, cctxParams.enableDedicatedDictSearch,
      customMem);

  if (!cdict || ZSTD_isError(ZSTD_initCDict_internal(
                    cdict, dict, dictSize, dictLoadMethod, dictContentType,
                    cctxParams))) {
    ZSTD_freeCDict(cdict);
    return NULL;
  }

  return cdict;
}

ZSTD_CDict *ZSTD_createCDict(const void *dict, size_t dictSize,
                             int compressionLevel) {
  ZSTD_compressionParameters cParams =
      ZSTD_getCParams_internal(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN,
                               dictSize, ZSTD_cpm_createCDict);
  ZSTD_CDict *const cdict =
      ZSTD_createCDict_advanced(dict, dictSize, ZSTD_dlm_byCopy, ZSTD_dct_auto,
                                cParams, ZSTD_defaultCMem);
  if (cdict)
    cdict->compressionLevel =
        (compressionLevel == 0) ? ZSTD_CLEVEL_DEFAULT : compressionLevel;
  return cdict;
}

ZSTD_CDict *ZSTD_createCDict_byReference(const void *dict, size_t dictSize,
                                         int compressionLevel) {
  ZSTD_compressionParameters cParams =
      ZSTD_getCParams_internal(compressionLevel, ZSTD_CONTENTSIZE_UNKNOWN,
                               dictSize, ZSTD_cpm_createCDict);
  ZSTD_CDict *const cdict = ZSTD_createCDict_advanced(
      dict, dictSize, ZSTD_dlm_byRef, ZSTD_dct_auto, cParams, ZSTD_defaultCMem);
  if (cdict)
    cdict->compressionLevel =
        (compressionLevel == 0) ? ZSTD_CLEVEL_DEFAULT : compressionLevel;
  return cdict;
}

size_t ZSTD_freeCDict(ZSTD_CDict *cdict) {
  if (cdict == NULL)
    return 0;
  {
    ZSTD_customMem const cMem = cdict->customMem;
    int cdictInWorkspace = ZSTD_cwksp_owns_buffer(&cdict->workspace, cdict);
    ZSTD_cwksp_free(&cdict->workspace, cMem);
    if (!cdictInWorkspace) {
      ZSTD_customFree(cdict, cMem);
    }
    return 0;
  }
}

const ZSTD_CDict *ZSTD_initStaticCDict(void *workspace, size_t workspaceSize,
                                       const void *dict, size_t dictSize,
                                       ZSTD_dictLoadMethod_e dictLoadMethod,
                                       ZSTD_dictContentType_e dictContentType,
                                       ZSTD_compressionParameters cParams) {
  ZSTD_ParamSwitch_e const useRowMatchFinder =
      ZSTD_resolveRowMatchFinderMode(ZSTD_ps_auto, &cParams);

  size_t const matchStateSize =
      ZSTD_sizeof_matchState(&cParams, useRowMatchFinder, 1, 0);
  size_t const neededSize =
      ZSTD_cwksp_alloc_size(sizeof(ZSTD_CDict)) +
      (dictLoadMethod == ZSTD_dlm_byRef
           ? 0
           : ZSTD_cwksp_alloc_size(
                 ZSTD_cwksp_align(dictSize, sizeof(void *)))) +
      ZSTD_cwksp_alloc_size(HUF_WORKSPACE_SIZE) + matchStateSize;
  ZSTD_CDict *cdict;
  ZSTD_CCtx_params params;

  DEBUGLOG(4, "ZSTD_initStaticCDict (dictSize==%u)", (unsigned)dictSize);
  if ((size_t)workspace & 7)
    return NULL;

  {
    ZSTD_cwksp ws;
    ZSTD_cwksp_init(&ws, workspace, workspaceSize, ZSTD_cwksp_static_alloc);
    cdict = (ZSTD_CDict *)ZSTD_cwksp_reserve_object(&ws, sizeof(ZSTD_CDict));
    if (cdict == NULL)
      return NULL;
    ZSTD_cwksp_move(&cdict->workspace, &ws);
  }

  if (workspaceSize < neededSize)
    return NULL;

  ZSTD_CCtxParams_init(&params, 0);
  params.cParams = cParams;
  params.useRowMatchFinder = useRowMatchFinder;
  cdict->useRowMatchFinder = useRowMatchFinder;
  cdict->compressionLevel = ZSTD_NO_CLEVEL;

  if (ZSTD_isError(ZSTD_initCDict_internal(
          cdict, dict, dictSize, dictLoadMethod, dictContentType, params)))
    return NULL;

  return cdict;
}

ZSTD_compressionParameters ZSTD_getCParamsFromCDict(const ZSTD_CDict *cdict) {
  assert(cdict != NULL);
  return cdict->matchState.cParams;
}

unsigned ZSTD_getDictID_fromCDict(const ZSTD_CDict *cdict) {
  if (cdict == NULL)
    return 0;
  return cdict->dictID;
}

static size_t ZSTD_compressBegin_usingCDict_internal(
    ZSTD_CCtx *const cctx, const ZSTD_CDict *const cdict,
    ZSTD_frameParameters const fParams,
    unsigned long long const pledgedSrcSize) {
  ZSTD_CCtx_params cctxParams;
  DEBUGLOG(4, "ZSTD_compressBegin_usingCDict_internal");
  RETURN_ERROR_IF(cdict == NULL, dictionary_wrong, "NULL pointer!");

  {
    ZSTD_parameters params;
    params.fParams = fParams;
    params.cParams =
        (pledgedSrcSize < ZSTD_USE_CDICT_PARAMS_SRCSIZE_CUTOFF ||
         pledgedSrcSize < cdict->dictContentSize *
                              ZSTD_USE_CDICT_PARAMS_DICTSIZE_MULTIPLIER ||
         pledgedSrcSize == ZSTD_CONTENTSIZE_UNKNOWN ||
         cdict->compressionLevel == 0)
            ? ZSTD_getCParamsFromCDict(cdict)
            : ZSTD_getCParams(cdict->compressionLevel, pledgedSrcSize,
                              cdict->dictContentSize);
    ZSTD_CCtxParams_init_internal(&cctxParams, &params,
                                  cdict->compressionLevel);
  }

  if (pledgedSrcSize != ZSTD_CONTENTSIZE_UNKNOWN) {
    U32 const limitedSrcSize = (U32)MIN(pledgedSrcSize, 1U << 19);
    U32 const limitedSrcLog =
        limitedSrcSize > 1 ? ZSTD_highbit32(limitedSrcSize - 1) + 1 : 1;
    cctxParams.cParams.windowLog =
        MAX(cctxParams.cParams.windowLog, limitedSrcLog);
  }
  return ZSTD_compressBegin_internal(cctx, NULL, 0, ZSTD_dct_auto,
                                     ZSTD_dtlm_fast, cdict, &cctxParams,
                                     pledgedSrcSize, ZSTDb_not_buffered);
}

size_t ZSTD_compressBegin_usingCDict_advanced(
    ZSTD_CCtx *const cctx, const ZSTD_CDict *const cdict,
    ZSTD_frameParameters const fParams,
    unsigned long long const pledgedSrcSize) {
  return ZSTD_compressBegin_usingCDict_internal(cctx, cdict, fParams,
                                                pledgedSrcSize);
}

size_t ZSTD_compressBegin_usingCDict_deprecated(ZSTD_CCtx *cctx,
                                                const ZSTD_CDict *cdict) {
  ZSTD_frameParameters const fParams = {0, 0, 0};
  return ZSTD_compressBegin_usingCDict_internal(cctx, cdict, fParams,
                                                ZSTD_CONTENTSIZE_UNKNOWN);
}

size_t ZSTD_compressBegin_usingCDict(ZSTD_CCtx *cctx, const ZSTD_CDict *cdict) {
  return ZSTD_compressBegin_usingCDict_deprecated(cctx, cdict);
}

static size_t ZSTD_compress_usingCDict_internal(ZSTD_CCtx *cctx, void *dst,
                                                size_t dstCapacity,
                                                const void *src, size_t srcSize,
                                                const ZSTD_CDict *cdict,
                                                ZSTD_frameParameters fParams) {
  FORWARD_IF_ERROR(
      ZSTD_compressBegin_usingCDict_internal(cctx, cdict, fParams, srcSize),
      "");
  return ZSTD_compressEnd_public(cctx, dst, dstCapacity, src, srcSize);
}

size_t ZSTD_compress_usingCDict_advanced(ZSTD_CCtx *cctx, void *dst,
                                         size_t dstCapacity, const void *src,
                                         size_t srcSize,
                                         const ZSTD_CDict *cdict,
                                         ZSTD_frameParameters fParams) {
  return ZSTD_compress_usingCDict_internal(cctx, dst, dstCapacity, src, srcSize,
                                           cdict, fParams);
}

size_t ZSTD_compress_usingCDict(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                                const void *src, size_t srcSize,
                                const ZSTD_CDict *cdict) {
  ZSTD_frameParameters const fParams = {1, 0, 0};
  return ZSTD_compress_usingCDict_internal(cctx, dst, dstCapacity, src, srcSize,
                                           cdict, fParams);
}

ZSTD_CStream *ZSTD_createCStream(void) {
  DEBUGLOG(3, "ZSTD_createCStream");
  return ZSTD_createCStream_advanced(ZSTD_defaultCMem);
}

ZSTD_CStream *ZSTD_initStaticCStream(void *workspace, size_t workspaceSize) {
  return ZSTD_initStaticCCtx(workspace, workspaceSize);
}

ZSTD_CStream *ZSTD_createCStream_advanced(ZSTD_customMem customMem) {
  return ZSTD_createCCtx_advanced(customMem);
}

size_t ZSTD_freeCStream(ZSTD_CStream *zcs) { return ZSTD_freeCCtx(zcs); }

size_t ZSTD_CStreamInSize(void) { return ZSTD_BLOCKSIZE_MAX; }

size_t ZSTD_CStreamOutSize(void) {
  return ZSTD_compressBound(ZSTD_BLOCKSIZE_MAX) + ZSTD_blockHeaderSize + 4;
}

static ZSTD_CParamMode_e ZSTD_getCParamMode(ZSTD_CDict const *cdict,
                                            ZSTD_CCtx_params const *params,
                                            U64 pledgedSrcSize) {
  if (cdict != NULL && ZSTD_shouldAttachDict(cdict, params, pledgedSrcSize))
    return ZSTD_cpm_attachDict;
  else
    return ZSTD_cpm_noAttachDict;
}

size_t ZSTD_resetCStream(ZSTD_CStream *zcs, unsigned long long pss) {

  U64 const pledgedSrcSize = (pss == 0) ? ZSTD_CONTENTSIZE_UNKNOWN : pss;
  DEBUGLOG(4, "ZSTD_resetCStream: pledgedSrcSize = %u",
           (unsigned)pledgedSrcSize);
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize), "");
  return 0;
}

size_t ZSTD_initCStream_internal(ZSTD_CStream *zcs, const void *dict,
                                 size_t dictSize, const ZSTD_CDict *cdict,
                                 const ZSTD_CCtx_params *params,
                                 unsigned long long pledgedSrcSize) {
  DEBUGLOG(4, "ZSTD_initCStream_internal");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize), "");
  assert(!ZSTD_isError(ZSTD_checkCParams(params->cParams)));
  zcs->requestedParams = *params;
  assert(!((dict) && (cdict)));
  if (dict) {
    FORWARD_IF_ERROR(ZSTD_CCtx_loadDictionary(zcs, dict, dictSize), "");
  } else {

    FORWARD_IF_ERROR(ZSTD_CCtx_refCDict(zcs, cdict), "");
  }
  return 0;
}

size_t ZSTD_initCStream_usingCDict_advanced(ZSTD_CStream *zcs,
                                            const ZSTD_CDict *cdict,
                                            ZSTD_frameParameters fParams,
                                            unsigned long long pledgedSrcSize) {
  DEBUGLOG(4, "ZSTD_initCStream_usingCDict_advanced");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize), "");
  zcs->requestedParams.fParams = fParams;
  FORWARD_IF_ERROR(ZSTD_CCtx_refCDict(zcs, cdict), "");
  return 0;
}

size_t ZSTD_initCStream_usingCDict(ZSTD_CStream *zcs, const ZSTD_CDict *cdict) {
  DEBUGLOG(4, "ZSTD_initCStream_usingCDict");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_refCDict(zcs, cdict), "");
  return 0;
}

size_t ZSTD_initCStream_advanced(ZSTD_CStream *zcs, const void *dict,
                                 size_t dictSize, ZSTD_parameters params,
                                 unsigned long long pss) {

  U64 const pledgedSrcSize = (pss == 0 && params.fParams.contentSizeFlag == 0)
                                 ? ZSTD_CONTENTSIZE_UNKNOWN
                                 : pss;
  DEBUGLOG(4, "ZSTD_initCStream_advanced");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize), "");
  FORWARD_IF_ERROR(ZSTD_checkCParams(params.cParams), "");
  ZSTD_CCtxParams_setZstdParams(&zcs->requestedParams, &params);
  FORWARD_IF_ERROR(ZSTD_CCtx_loadDictionary(zcs, dict, dictSize), "");
  return 0;
}

size_t ZSTD_initCStream_usingDict(ZSTD_CStream *zcs, const void *dict,
                                  size_t dictSize, int compressionLevel) {
  DEBUGLOG(4, "ZSTD_initCStream_usingDict");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel),
      "");
  FORWARD_IF_ERROR(ZSTD_CCtx_loadDictionary(zcs, dict, dictSize), "");
  return 0;
}

size_t ZSTD_initCStream_srcSize(ZSTD_CStream *zcs, int compressionLevel,
                                unsigned long long pss) {

  U64 const pledgedSrcSize = (pss == 0) ? ZSTD_CONTENTSIZE_UNKNOWN : pss;
  DEBUGLOG(4, "ZSTD_initCStream_srcSize");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_refCDict(zcs, NULL), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel),
      "");
  FORWARD_IF_ERROR(ZSTD_CCtx_setPledgedSrcSize(zcs, pledgedSrcSize), "");
  return 0;
}

size_t ZSTD_initCStream(ZSTD_CStream *zcs, int compressionLevel) {
  DEBUGLOG(4, "ZSTD_initCStream");
  FORWARD_IF_ERROR(ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only), "");
  FORWARD_IF_ERROR(ZSTD_CCtx_refCDict(zcs, NULL), "");
  FORWARD_IF_ERROR(
      ZSTD_CCtx_setParameter(zcs, ZSTD_c_compressionLevel, compressionLevel),
      "");
  return 0;
}

static size_t ZSTD_nextInputSizeHint(const ZSTD_CCtx *cctx) {
  if (cctx->appliedParams.inBufferMode == ZSTD_bm_stable) {
    return cctx->blockSizeMax - cctx->stableIn_notConsumed;
  }
  assert(cctx->appliedParams.inBufferMode == ZSTD_bm_buffered);
  {
    size_t hintInSize = cctx->inBuffTarget - cctx->inBuffPos;
    if (hintInSize == 0)
      hintInSize = cctx->blockSizeMax;
    return hintInSize;
  }
}

static size_t ZSTD_compressStream_generic(ZSTD_CStream *zcs,
                                          ZSTD_outBuffer *output,
                                          ZSTD_inBuffer *input,
                                          ZSTD_EndDirective const flushMode) {
  const char *const istart = (assert(input != NULL), (const char *)input->src);
  const char *const iend = (istart != NULL) ? istart + input->size : istart;
  const char *ip = (istart != NULL) ? istart + input->pos : istart;
  char *const ostart = (assert(output != NULL), (char *)output->dst);
  char *const oend = (ostart != NULL) ? ostart + output->size : ostart;
  char *op = (ostart != NULL) ? ostart + output->pos : ostart;
  U32 someMoreWork = 1;

  DEBUGLOG(5, "ZSTD_compressStream_generic, flush=%i, srcSize = %zu",
           (int)flushMode, input->size - input->pos);
  assert(zcs != NULL);
  if (zcs->appliedParams.inBufferMode == ZSTD_bm_stable) {
    assert(input->pos >= zcs->stableIn_notConsumed);
    input->pos -= zcs->stableIn_notConsumed;
    if (ip)
      ip -= zcs->stableIn_notConsumed;
    zcs->stableIn_notConsumed = 0;
  }
  if (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered) {
    assert(zcs->inBuff != NULL);
    assert(zcs->inBuffSize > 0);
  }
  if (zcs->appliedParams.outBufferMode == ZSTD_bm_buffered) {
    assert(zcs->outBuff != NULL);
    assert(zcs->outBuffSize > 0);
  }
  if (input->src == NULL)
    assert(input->size == 0);
  assert(input->pos <= input->size);
  if (output->dst == NULL)
    assert(output->size == 0);
  assert(output->pos <= output->size);
  assert((U32)flushMode <= (U32)ZSTD_e_end);

  while (someMoreWork) {
    switch (zcs->streamStage) {
    case zcss_init:
      RETURN_ERROR(init_missing, "call ZSTD_initCStream() first!");

    case zcss_load:
      if ((flushMode == ZSTD_e_end) &&
          ((size_t)(oend - op) >= ZSTD_compressBound((size_t)(iend - ip)) ||
           zcs->appliedParams.outBufferMode == ZSTD_bm_stable) &&
          (zcs->inBuffPos == 0)) {

        size_t const cSize = ZSTD_compressEnd_public(
            zcs, op, (size_t)(oend - op), ip, (size_t)(iend - ip));
        DEBUGLOG(4, "ZSTD_compressEnd : cSize=%u", (unsigned)cSize);
        FORWARD_IF_ERROR(cSize, "ZSTD_compressEnd failed");
        ip = iend;
        op += cSize;
        zcs->frameEnded = 1;
        ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
        someMoreWork = 0;
        break;
      }

      if (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered) {
        size_t const toLoad = zcs->inBuffTarget - zcs->inBuffPos;
        size_t const loaded = ZSTD_limitCopy(zcs->inBuff + zcs->inBuffPos,
                                             toLoad, ip, (size_t)(iend - ip));
        zcs->inBuffPos += loaded;
        if (ip)
          ip += loaded;
        if ((flushMode == ZSTD_e_continue) &&
            (zcs->inBuffPos < zcs->inBuffTarget)) {

          someMoreWork = 0;
          break;
        }
        if ((flushMode == ZSTD_e_flush) &&
            (zcs->inBuffPos == zcs->inToCompress)) {

          someMoreWork = 0;
          break;
        }
      } else {
        assert(zcs->appliedParams.inBufferMode == ZSTD_bm_stable);
        if ((flushMode == ZSTD_e_continue) &&
            ((size_t)(iend - ip) < zcs->blockSizeMax)) {

          zcs->stableIn_notConsumed = (size_t)(iend - ip);
          ip = iend;
          someMoreWork = 0;
          break;
        }
        if ((flushMode == ZSTD_e_flush) && (ip == iend)) {

          someMoreWork = 0;
          break;
        }
      }

      DEBUGLOG(5, "stream compression stage (flushMode==%u)", flushMode);
      {
        int const inputBuffered =
            (zcs->appliedParams.inBufferMode == ZSTD_bm_buffered);
        void *cDst;
        size_t cSize;
        size_t oSize = (size_t)(oend - op);
        size_t const iSize = inputBuffered
                                 ? zcs->inBuffPos - zcs->inToCompress
                                 : MIN((size_t)(iend - ip), zcs->blockSizeMax);
        if (oSize >= ZSTD_compressBound(iSize) ||
            zcs->appliedParams.outBufferMode == ZSTD_bm_stable)
          cDst = op;
        else
          cDst = zcs->outBuff, oSize = zcs->outBuffSize;
        if (inputBuffered) {
          unsigned const lastBlock = (flushMode == ZSTD_e_end) && (ip == iend);
          cSize = lastBlock
                      ? ZSTD_compressEnd_public(zcs, cDst, oSize,
                                                zcs->inBuff + zcs->inToCompress,
                                                iSize)
                      : ZSTD_compressContinue_public(
                            zcs, cDst, oSize, zcs->inBuff + zcs->inToCompress,
                            iSize);
          FORWARD_IF_ERROR(cSize, "%s",
                           lastBlock ? "ZSTD_compressEnd failed"
                                     : "ZSTD_compressContinue failed");
          zcs->frameEnded = lastBlock;

          zcs->inBuffTarget = zcs->inBuffPos + zcs->blockSizeMax;
          if (zcs->inBuffTarget > zcs->inBuffSize)
            zcs->inBuffPos = 0, zcs->inBuffTarget = zcs->blockSizeMax;
          DEBUGLOG(5, "inBuffTarget:%u / inBuffSize:%u",
                   (unsigned)zcs->inBuffTarget, (unsigned)zcs->inBuffSize);
          if (!lastBlock)
            assert(zcs->inBuffTarget <= zcs->inBuffSize);
          zcs->inToCompress = zcs->inBuffPos;
        } else {
          unsigned const lastBlock =
              (flushMode == ZSTD_e_end) && (ip + iSize == iend);
          cSize =
              lastBlock
                  ? ZSTD_compressEnd_public(zcs, cDst, oSize, ip, iSize)
                  : ZSTD_compressContinue_public(zcs, cDst, oSize, ip, iSize);

          if (ip)
            ip += iSize;
          FORWARD_IF_ERROR(cSize, "%s",
                           lastBlock ? "ZSTD_compressEnd failed"
                                     : "ZSTD_compressContinue failed");
          zcs->frameEnded = lastBlock;
          if (lastBlock)
            assert(ip == iend);
        }
        if (cDst == op) {
          op += cSize;
          if (zcs->frameEnded) {
            DEBUGLOG(5, "Frame completed directly in outBuffer");
            someMoreWork = 0;
            ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
          }
          break;
        }
        zcs->outBuffContentSize = cSize;
        zcs->outBuffFlushedSize = 0;
        zcs->streamStage = zcss_flush;
      }
      ZSTD_FALLTHROUGH;
    case zcss_flush:
      DEBUGLOG(5, "flush stage");
      assert(zcs->appliedParams.outBufferMode == ZSTD_bm_buffered);
      {
        size_t const toFlush =
            zcs->outBuffContentSize - zcs->outBuffFlushedSize;
        size_t const flushed =
            ZSTD_limitCopy(op, (size_t)(oend - op),
                           zcs->outBuff + zcs->outBuffFlushedSize, toFlush);
        DEBUGLOG(5, "toFlush: %u into %u ==> flushed: %u", (unsigned)toFlush,
                 (unsigned)(oend - op), (unsigned)flushed);
        if (flushed)
          op += flushed;
        zcs->outBuffFlushedSize += flushed;
        if (toFlush != flushed) {

          assert(op == oend);
          someMoreWork = 0;
          break;
        }
        zcs->outBuffContentSize = zcs->outBuffFlushedSize = 0;
        if (zcs->frameEnded) {
          DEBUGLOG(5, "Frame completed on flush");
          someMoreWork = 0;
          ZSTD_CCtx_reset(zcs, ZSTD_reset_session_only);
          break;
        }
        zcs->streamStage = zcss_load;
        break;
      }

    default:
      assert(0);
    }
  }

  input->pos = (size_t)(ip - istart);
  output->pos = (size_t)(op - ostart);
  if (zcs->frameEnded)
    return 0;
  return ZSTD_nextInputSizeHint(zcs);
}

static size_t ZSTD_nextInputSizeHint_MTorST(const ZSTD_CCtx *cctx) {
#ifdef ZSTD_MULTITHREAD
  if (cctx->appliedParams.nbWorkers >= 1) {
    assert(cctx->mtctx != NULL);
    return ZSTDMT_nextInputSizeHint(cctx->mtctx);
  }
#endif
  return ZSTD_nextInputSizeHint(cctx);
}

size_t ZSTD_compressStream(ZSTD_CStream *zcs, ZSTD_outBuffer *output,
                           ZSTD_inBuffer *input) {
  FORWARD_IF_ERROR(ZSTD_compressStream2(zcs, output, input, ZSTD_e_continue),
                   "");
  return ZSTD_nextInputSizeHint_MTorST(zcs);
}

static void ZSTD_setBufferExpectations(ZSTD_CCtx *cctx,
                                       const ZSTD_outBuffer *output,
                                       const ZSTD_inBuffer *input) {
  DEBUGLOG(5, "ZSTD_setBufferExpectations (for advanced stable in/out modes)");
  if (cctx->appliedParams.inBufferMode == ZSTD_bm_stable) {
    cctx->expectedInBuffer = *input;
  }
  if (cctx->appliedParams.outBufferMode == ZSTD_bm_stable) {
    cctx->expectedOutBufferSize = output->size - output->pos;
  }
}

static size_t ZSTD_checkBufferStability(ZSTD_CCtx const *cctx,
                                        ZSTD_outBuffer const *output,
                                        ZSTD_inBuffer const *input,
                                        ZSTD_EndDirective endOp) {
  if (cctx->appliedParams.inBufferMode == ZSTD_bm_stable) {
    ZSTD_inBuffer const expect = cctx->expectedInBuffer;
    if (expect.src != input->src || expect.pos != input->pos)
      RETURN_ERROR(stabilityCondition_notRespected,
                   "ZSTD_c_stableInBuffer enabled but input differs!");
  }
  (void)endOp;
  if (cctx->appliedParams.outBufferMode == ZSTD_bm_stable) {
    size_t const outBufferSize = output->size - output->pos;
    if (cctx->expectedOutBufferSize != outBufferSize)
      RETURN_ERROR(stabilityCondition_notRespected,
                   "ZSTD_c_stableOutBuffer enabled but output size differs!");
  }
  return 0;
}

static size_t ZSTD_CCtx_init_compressStream2(ZSTD_CCtx *cctx,
                                             ZSTD_EndDirective endOp,
                                             size_t inSize) {
  ZSTD_CCtx_params params = cctx->requestedParams;
  ZSTD_prefixDict const prefixDict = cctx->prefixDict;
  FORWARD_IF_ERROR(ZSTD_initLocalDict(cctx), "");
  ZSTD_memset(&cctx->prefixDict, 0, sizeof(cctx->prefixDict));
  assert(prefixDict.dict == NULL || cctx->cdict == NULL);
  if (cctx->cdict && !cctx->localDict.cdict) {

    params.compressionLevel = cctx->cdict->compressionLevel;
  }
  DEBUGLOG(4, "ZSTD_CCtx_init_compressStream2 : transparent init stage");
  if (endOp == ZSTD_e_end)
    cctx->pledgedSrcSizePlusOne = inSize + 1;

  {
    size_t const dictSize =
        prefixDict.dict ? prefixDict.dictSize
                        : (cctx->cdict ? cctx->cdict->dictContentSize : 0);
    ZSTD_CParamMode_e const mode = ZSTD_getCParamMode(
        cctx->cdict, &params, cctx->pledgedSrcSizePlusOne - 1);
    params.cParams = ZSTD_getCParamsFromCCtxParams(
        &params, cctx->pledgedSrcSizePlusOne - 1, dictSize, mode);
  }

  params.postBlockSplitter =
      ZSTD_resolveBlockSplitterMode(params.postBlockSplitter, &params.cParams);
  params.ldmParams.enableLdm =
      ZSTD_resolveEnableLdm(params.ldmParams.enableLdm, &params.cParams);
  params.useRowMatchFinder =
      ZSTD_resolveRowMatchFinderMode(params.useRowMatchFinder, &params.cParams);
  params.validateSequences =
      ZSTD_resolveExternalSequenceValidation(params.validateSequences);
  params.maxBlockSize = ZSTD_resolveMaxBlockSize(params.maxBlockSize);
  params.searchForExternalRepcodes = ZSTD_resolveExternalRepcodeSearch(
      params.searchForExternalRepcodes, params.compressionLevel);

#ifdef ZSTD_MULTITHREAD

  RETURN_ERROR_IF(
      ZSTD_hasExtSeqProd(&params) && params.nbWorkers >= 1,
      parameter_combination_unsupported,
      "External sequence producer isn't supported with nbWorkers >= 1");

  if ((cctx->pledgedSrcSizePlusOne - 1) <= ZSTDMT_JOBSIZE_MIN) {
    params.nbWorkers = 0;
  }
  if (params.nbWorkers > 0) {
#if ZSTD_TRACE
    cctx->traceCtx = (ZSTD_trace_compress_begin != NULL)
                         ? ZSTD_trace_compress_begin(cctx)
                         : 0;
#endif

    if (cctx->mtctx == NULL) {
      DEBUGLOG(4, "ZSTD_compressStream2: creating new mtctx for nbWorkers=%u",
               params.nbWorkers);
      cctx->mtctx = ZSTDMT_createCCtx_advanced((U32)params.nbWorkers,
                                               cctx->customMem, cctx->pool);
      RETURN_ERROR_IF(cctx->mtctx == NULL, memory_allocation, "NULL pointer!");
    }

    DEBUGLOG(4, "call ZSTDMT_initCStream_internal as nbWorkers=%u",
             params.nbWorkers);
    FORWARD_IF_ERROR(ZSTDMT_initCStream_internal(
                         cctx->mtctx, prefixDict.dict, prefixDict.dictSize,
                         prefixDict.dictContentType, cctx->cdict, params,
                         cctx->pledgedSrcSizePlusOne - 1),
                     "");
    cctx->dictID = cctx->cdict ? cctx->cdict->dictID : 0;
    cctx->dictContentSize =
        cctx->cdict ? cctx->cdict->dictContentSize : prefixDict.dictSize;
    cctx->consumedSrcSize = 0;
    cctx->producedCSize = 0;
    cctx->streamStage = zcss_load;
    cctx->appliedParams = params;
  } else
#endif
  {
    U64 const pledgedSrcSize = cctx->pledgedSrcSizePlusOne - 1;
    assert(!ZSTD_isError(ZSTD_checkCParams(params.cParams)));
    FORWARD_IF_ERROR(ZSTD_compressBegin_internal(
                         cctx, prefixDict.dict, prefixDict.dictSize,
                         prefixDict.dictContentType, ZSTD_dtlm_fast,
                         cctx->cdict, &params, pledgedSrcSize, ZSTDb_buffered),
                     "");
    assert(cctx->appliedParams.nbWorkers == 0);
    cctx->inToCompress = 0;
    cctx->inBuffPos = 0;
    if (cctx->appliedParams.inBufferMode == ZSTD_bm_buffered) {

      cctx->inBuffTarget =
          cctx->blockSizeMax + (cctx->blockSizeMax == pledgedSrcSize);
    } else {
      cctx->inBuffTarget = 0;
    }
    cctx->outBuffContentSize = cctx->outBuffFlushedSize = 0;
    cctx->streamStage = zcss_load;
    cctx->frameEnded = 0;
  }
  return 0;
}

size_t ZSTD_compressStream2(ZSTD_CCtx *cctx, ZSTD_outBuffer *output,
                            ZSTD_inBuffer *input, ZSTD_EndDirective endOp) {
  DEBUGLOG(5, "ZSTD_compressStream2, endOp=%u ", (unsigned)endOp);

  RETURN_ERROR_IF(output->pos > output->size, dstSize_tooSmall,
                  "invalid output buffer");
  RETURN_ERROR_IF(input->pos > input->size, srcSize_wrong,
                  "invalid input buffer");
  RETURN_ERROR_IF((U32)endOp > (U32)ZSTD_e_end, parameter_outOfBound,
                  "invalid endDirective");
  assert(cctx != NULL);

  if (cctx->streamStage == zcss_init) {
    size_t const inputSize = input->size - input->pos;
    size_t const totalInputSize = inputSize + cctx->stableIn_notConsumed;
    if ((cctx->requestedParams.inBufferMode == ZSTD_bm_stable) &&
        (endOp == ZSTD_e_continue) && (totalInputSize < ZSTD_BLOCKSIZE_MAX)) {
      if (cctx->stableIn_notConsumed) {

        RETURN_ERROR_IF(
            input->src != cctx->expectedInBuffer.src,
            stabilityCondition_notRespected,
            "stableInBuffer condition not respected: wrong src pointer");
        RETURN_ERROR_IF(
            input->pos != cctx->expectedInBuffer.size,
            stabilityCondition_notRespected,
            "stableInBuffer condition not respected: externally modified pos");
      }

      input->pos = input->size;

      cctx->expectedInBuffer = *input;

      cctx->stableIn_notConsumed += inputSize;

      return ZSTD_FRAMEHEADERSIZE_MIN(cctx->requestedParams.format);
    }
    FORWARD_IF_ERROR(
        ZSTD_CCtx_init_compressStream2(cctx, endOp, totalInputSize),
        "compressStream2 initialization failed");
    ZSTD_setBufferExpectations(cctx, output, input);
  }

  FORWARD_IF_ERROR(ZSTD_checkBufferStability(cctx, output, input, endOp),
                   "invalid buffers");

#ifdef ZSTD_MULTITHREAD
  if (cctx->appliedParams.nbWorkers > 0) {
    size_t flushMin;
    if (cctx->cParamsChanged) {
      ZSTDMT_updateCParams_whileCompressing(cctx->mtctx,
                                            &cctx->requestedParams);
      cctx->cParamsChanged = 0;
    }
    if (cctx->stableIn_notConsumed) {
      assert(cctx->appliedParams.inBufferMode == ZSTD_bm_stable);

      assert(input->pos >= cctx->stableIn_notConsumed);
      input->pos -= cctx->stableIn_notConsumed;
      cctx->stableIn_notConsumed = 0;
    }
    for (;;) {
      size_t const ipos = input->pos;
      size_t const opos = output->pos;
      flushMin =
          ZSTDMT_compressStream_generic(cctx->mtctx, output, input, endOp);
      cctx->consumedSrcSize += (U64)(input->pos - ipos);
      cctx->producedCSize += (U64)(output->pos - opos);
      if (ZSTD_isError(flushMin) || (endOp == ZSTD_e_end && flushMin == 0)) {
        if (flushMin == 0)
          ZSTD_CCtx_trace(cctx, 0);
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
      }
      FORWARD_IF_ERROR(flushMin, "ZSTDMT_compressStream_generic failed");

      if (endOp == ZSTD_e_continue) {

        if (input->pos != ipos || output->pos != opos ||
            input->pos == input->size || output->pos == output->size)
          break;
      } else {
        assert(endOp == ZSTD_e_flush || endOp == ZSTD_e_end);

        if (flushMin == 0 || output->pos == output->size)
          break;
      }
    }
    DEBUGLOG(5, "completed ZSTD_compressStream2 delegating to "
                "ZSTDMT_compressStream_generic");

    assert(endOp == ZSTD_e_continue || flushMin == 0 ||
           output->pos == output->size);
    ZSTD_setBufferExpectations(cctx, output, input);
    return flushMin;
  }
#endif
  FORWARD_IF_ERROR(ZSTD_compressStream_generic(cctx, output, input, endOp), "");
  DEBUGLOG(5, "completed ZSTD_compressStream2");
  ZSTD_setBufferExpectations(cctx, output, input);
  return cctx->outBuffContentSize - cctx->outBuffFlushedSize;
}

size_t ZSTD_compressStream2_simpleArgs(ZSTD_CCtx *cctx, void *dst,
                                       size_t dstCapacity, size_t *dstPos,
                                       const void *src, size_t srcSize,
                                       size_t *srcPos,
                                       ZSTD_EndDirective endOp) {
  ZSTD_outBuffer output;
  ZSTD_inBuffer input;
  output.dst = dst;
  output.size = dstCapacity;
  output.pos = *dstPos;
  input.src = src;
  input.size = srcSize;
  input.pos = *srcPos;

  {
    size_t const cErr = ZSTD_compressStream2(cctx, &output, &input, endOp);
    *dstPos = output.pos;
    *srcPos = input.pos;
    return cErr;
  }
}

size_t ZSTD_compress2(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                      const void *src, size_t srcSize) {
  ZSTD_bufferMode_e const originalInBufferMode =
      cctx->requestedParams.inBufferMode;
  ZSTD_bufferMode_e const originalOutBufferMode =
      cctx->requestedParams.outBufferMode;
  DEBUGLOG(4, "ZSTD_compress2 (srcSize=%u)", (unsigned)srcSize);
  ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);

  cctx->requestedParams.inBufferMode = ZSTD_bm_stable;
  cctx->requestedParams.outBufferMode = ZSTD_bm_stable;
  {
    size_t oPos = 0;
    size_t iPos = 0;
    size_t const result = ZSTD_compressStream2_simpleArgs(
        cctx, dst, dstCapacity, &oPos, src, srcSize, &iPos, ZSTD_e_end);

    cctx->requestedParams.inBufferMode = originalInBufferMode;
    cctx->requestedParams.outBufferMode = originalOutBufferMode;

    FORWARD_IF_ERROR(result, "ZSTD_compressStream2_simpleArgs failed");
    if (result != 0) {
      assert(oPos == dstCapacity);
      RETURN_ERROR(dstSize_tooSmall, "");
    }
    assert(iPos == srcSize);
    return oPos;
  }
}

static size_t ZSTD_validateSequence(U32 offBase, U32 matchLength, U32 minMatch,
                                    size_t posInSrc, U32 windowLog,
                                    size_t dictSize, int useSequenceProducer) {
  U32 const windowSize = 1u << windowLog;

  size_t const offsetBound =
      posInSrc > windowSize ? (size_t)windowSize : posInSrc + (size_t)dictSize;
  size_t const matchLenLowerBound =
      (minMatch == 3 || useSequenceProducer) ? 3 : 4;
  RETURN_ERROR_IF(offBase > OFFSET_TO_OFFBASE(offsetBound),
                  externalSequences_invalid, "Offset too large!");

  RETURN_ERROR_IF(matchLength < matchLenLowerBound, externalSequences_invalid,
                  "Matchlength too small for the minMatch");
  return 0;
}

static U32 ZSTD_finalizeOffBase(U32 rawOffset, const U32 rep[ZSTD_REP_NUM],
                                U32 ll0) {
  U32 offBase = OFFSET_TO_OFFBASE(rawOffset);

  if (!ll0 && rawOffset == rep[0]) {
    offBase = REPCODE1_TO_OFFBASE;
  } else if (rawOffset == rep[1]) {
    offBase = REPCODE_TO_OFFBASE(2 - ll0);
  } else if (rawOffset == rep[2]) {
    offBase = REPCODE_TO_OFFBASE(3 - ll0);
  } else if (ll0 && rawOffset == rep[0] - 1) {
    offBase = REPCODE3_TO_OFFBASE;
  }
  return offBase;
}

static size_t ZSTD_transferSequences_wBlockDelim(
    ZSTD_CCtx *cctx, ZSTD_SequencePosition *seqPos, const ZSTD_Sequence *inSeqs,
    size_t inSeqsSize, const void *src, size_t blockSize,
    ZSTD_ParamSwitch_e externalRepSearch) {
  U32 idx = seqPos->idx;
  U32 const startIdx = idx;
  BYTE const *ip = (BYTE const *)(src);
  const BYTE *const iend = ip + blockSize;
  Repcodes_t updatedRepcodes;
  U32 dictSize;

  DEBUGLOG(5, "ZSTD_transferSequences_wBlockDelim (blockSize = %zu)",
           blockSize);

  if (cctx->cdict) {
    dictSize = (U32)cctx->cdict->dictContentSize;
  } else if (cctx->prefixDict.dict) {
    dictSize = (U32)cctx->prefixDict.dictSize;
  } else {
    dictSize = 0;
  }
  ZSTD_memcpy(updatedRepcodes.rep, cctx->blockState.prevCBlock->rep,
              sizeof(Repcodes_t));
  for (; idx < inSeqsSize &&
         (inSeqs[idx].matchLength != 0 || inSeqs[idx].offset != 0);
       ++idx) {
    U32 const litLength = inSeqs[idx].litLength;
    U32 const matchLength = inSeqs[idx].matchLength;
    U32 offBase;

    if (externalRepSearch == ZSTD_ps_disable) {
      offBase = OFFSET_TO_OFFBASE(inSeqs[idx].offset);
    } else {
      U32 const ll0 = (litLength == 0);
      offBase =
          ZSTD_finalizeOffBase(inSeqs[idx].offset, updatedRepcodes.rep, ll0);
      ZSTD_updateRep(updatedRepcodes.rep, offBase, ll0);
    }

    DEBUGLOG(6, "Storing sequence: (of: %u, ml: %u, ll: %u)", offBase,
             matchLength, litLength);
    if (cctx->appliedParams.validateSequences) {
      seqPos->posInSrc += litLength + matchLength;
      FORWARD_IF_ERROR(
          ZSTD_validateSequence(
              offBase, matchLength, cctx->appliedParams.cParams.minMatch,
              seqPos->posInSrc, cctx->appliedParams.cParams.windowLog, dictSize,
              ZSTD_hasExtSeqProd(&cctx->appliedParams)),
          "Sequence validation failed");
    }
    RETURN_ERROR_IF(
        idx - seqPos->idx >= cctx->seqStore.maxNbSeq, externalSequences_invalid,
        "Not enough memory allocated. Try adjusting ZSTD_c_minMatch.");
    ZSTD_storeSeq(&cctx->seqStore, litLength, ip, iend, offBase, matchLength);
    ip += matchLength + litLength;
  }
  RETURN_ERROR_IF(idx == inSeqsSize, externalSequences_invalid,
                  "Block delimiter not found.");

  assert(externalRepSearch != ZSTD_ps_auto);
  assert(idx >= startIdx);
  if (externalRepSearch == ZSTD_ps_disable && idx != startIdx) {
    U32 *const rep = updatedRepcodes.rep;
    U32 lastSeqIdx = idx - 1;

    if (lastSeqIdx >= startIdx + 2) {
      rep[2] = inSeqs[lastSeqIdx - 2].offset;
      rep[1] = inSeqs[lastSeqIdx - 1].offset;
      rep[0] = inSeqs[lastSeqIdx].offset;
    } else if (lastSeqIdx == startIdx + 1) {
      rep[2] = rep[0];
      rep[1] = inSeqs[lastSeqIdx - 1].offset;
      rep[0] = inSeqs[lastSeqIdx].offset;
    } else {
      assert(lastSeqIdx == startIdx);
      rep[2] = rep[1];
      rep[1] = rep[0];
      rep[0] = inSeqs[lastSeqIdx].offset;
    }
  }

  ZSTD_memcpy(cctx->blockState.nextCBlock->rep, updatedRepcodes.rep,
              sizeof(Repcodes_t));

  if (inSeqs[idx].litLength) {
    DEBUGLOG(6, "Storing last literals of size: %u", inSeqs[idx].litLength);
    ZSTD_storeLastLiterals(&cctx->seqStore, ip, inSeqs[idx].litLength);
    ip += inSeqs[idx].litLength;
    seqPos->posInSrc += inSeqs[idx].litLength;
  }
  RETURN_ERROR_IF(ip != iend, externalSequences_invalid,
                  "Blocksize doesn't agree with block delimiter!");
  seqPos->idx = idx + 1;
  return blockSize;
}

static size_t
ZSTD_transferSequences_noDelim(ZSTD_CCtx *cctx, ZSTD_SequencePosition *seqPos,
                               const ZSTD_Sequence *inSeqs, size_t inSeqsSize,
                               const void *src, size_t blockSize,
                               ZSTD_ParamSwitch_e externalRepSearch) {
  U32 idx = seqPos->idx;
  U32 startPosInSequence = seqPos->posInSequence;
  U32 endPosInSequence = seqPos->posInSequence + (U32)blockSize;
  size_t dictSize;
  const BYTE *const istart = (const BYTE *)(src);
  const BYTE *ip = istart;
  const BYTE *iend = istart + blockSize;

  Repcodes_t updatedRepcodes;
  U32 bytesAdjustment = 0;
  U32 finalMatchSplit = 0;

  (void)externalRepSearch;

  if (cctx->cdict) {
    dictSize = cctx->cdict->dictContentSize;
  } else if (cctx->prefixDict.dict) {
    dictSize = cctx->prefixDict.dictSize;
  } else {
    dictSize = 0;
  }
  DEBUGLOG(5, "ZSTD_transferSequences_noDelim: idx: %u PIS: %u blockSize: %zu",
           idx, startPosInSequence, blockSize);
  DEBUGLOG(5, "Start seq: idx: %u (of: %u ml: %u ll: %u)", idx,
           inSeqs[idx].offset, inSeqs[idx].matchLength, inSeqs[idx].litLength);
  ZSTD_memcpy(updatedRepcodes.rep, cctx->blockState.prevCBlock->rep,
              sizeof(Repcodes_t));
  while (endPosInSequence && idx < inSeqsSize && !finalMatchSplit) {
    const ZSTD_Sequence currSeq = inSeqs[idx];
    U32 litLength = currSeq.litLength;
    U32 matchLength = currSeq.matchLength;
    U32 const rawOffset = currSeq.offset;
    U32 offBase;

    if (endPosInSequence >= currSeq.litLength + currSeq.matchLength) {
      if (startPosInSequence >= litLength) {
        startPosInSequence -= litLength;
        litLength = 0;
        matchLength -= startPosInSequence;
      } else {
        litLength -= startPosInSequence;
      }

      endPosInSequence -= currSeq.litLength + currSeq.matchLength;
      startPosInSequence = 0;
    } else {

      DEBUGLOG(6, "Require a split: diff: %u, idx: %u PIS: %u",
               currSeq.litLength + currSeq.matchLength - endPosInSequence, idx,
               endPosInSequence);
      if (endPosInSequence > litLength) {
        U32 firstHalfMatchLength;
        litLength = startPosInSequence >= litLength
                        ? 0
                        : litLength - startPosInSequence;
        firstHalfMatchLength =
            endPosInSequence - startPosInSequence - litLength;
        if (matchLength > blockSize &&
            firstHalfMatchLength >= cctx->appliedParams.cParams.minMatch) {

          U32 secondHalfMatchLength =
              currSeq.matchLength + currSeq.litLength - endPosInSequence;
          if (secondHalfMatchLength < cctx->appliedParams.cParams.minMatch) {

            endPosInSequence -=
                cctx->appliedParams.cParams.minMatch - secondHalfMatchLength;
            bytesAdjustment =
                cctx->appliedParams.cParams.minMatch - secondHalfMatchLength;
            firstHalfMatchLength -= bytesAdjustment;
          }
          matchLength = firstHalfMatchLength;

          finalMatchSplit = 1;
        } else {

          bytesAdjustment = endPosInSequence - currSeq.litLength;
          endPosInSequence = currSeq.litLength;
          break;
        }
      } else {

        break;
      }
    }

    {
      U32 const ll0 = (litLength == 0);
      offBase = ZSTD_finalizeOffBase(rawOffset, updatedRepcodes.rep, ll0);
      ZSTD_updateRep(updatedRepcodes.rep, offBase, ll0);
    }

    if (cctx->appliedParams.validateSequences) {
      seqPos->posInSrc += litLength + matchLength;
      FORWARD_IF_ERROR(
          ZSTD_validateSequence(
              offBase, matchLength, cctx->appliedParams.cParams.minMatch,
              seqPos->posInSrc, cctx->appliedParams.cParams.windowLog, dictSize,
              ZSTD_hasExtSeqProd(&cctx->appliedParams)),
          "Sequence validation failed");
    }
    DEBUGLOG(6, "Storing sequence: (of: %u, ml: %u, ll: %u)", offBase,
             matchLength, litLength);
    RETURN_ERROR_IF(
        idx - seqPos->idx >= cctx->seqStore.maxNbSeq, externalSequences_invalid,
        "Not enough memory allocated. Try adjusting ZSTD_c_minMatch.");
    ZSTD_storeSeq(&cctx->seqStore, litLength, ip, iend, offBase, matchLength);
    ip += matchLength + litLength;
    if (!finalMatchSplit)
      idx++;
  }
  DEBUGLOG(5, "Ending seq: idx: %u (of: %u ml: %u ll: %u)", idx,
           inSeqs[idx].offset, inSeqs[idx].matchLength, inSeqs[idx].litLength);
  assert(idx == inSeqsSize ||
         endPosInSequence <= inSeqs[idx].litLength + inSeqs[idx].matchLength);
  seqPos->idx = idx;
  seqPos->posInSequence = endPosInSequence;
  ZSTD_memcpy(cctx->blockState.nextCBlock->rep, updatedRepcodes.rep,
              sizeof(Repcodes_t));

  iend -= bytesAdjustment;
  if (ip != iend) {

    U32 const lastLLSize = (U32)(iend - ip);
    assert(ip <= iend);
    DEBUGLOG(6, "Storing last literals of size: %u", lastLLSize);
    ZSTD_storeLastLiterals(&cctx->seqStore, ip, lastLLSize);
    seqPos->posInSrc += lastLLSize;
  }

  return (size_t)(iend - istart);
}

typedef size_t (*ZSTD_SequenceCopier_f)(ZSTD_CCtx *cctx,
                                        ZSTD_SequencePosition *seqPos,
                                        const ZSTD_Sequence *inSeqs,
                                        size_t inSeqsSize, const void *src,
                                        size_t blockSize,
                                        ZSTD_ParamSwitch_e externalRepSearch);

static ZSTD_SequenceCopier_f
ZSTD_selectSequenceCopier(ZSTD_SequenceFormat_e mode) {
  assert(ZSTD_cParam_withinBounds(ZSTD_c_blockDelimiters, (int)mode));
  if (mode == ZSTD_sf_explicitBlockDelimiters) {
    return ZSTD_transferSequences_wBlockDelim;
  }
  assert(mode == ZSTD_sf_noBlockDelimiters);
  return ZSTD_transferSequences_noDelim;
}

static size_t blockSize_explicitDelimiter(const ZSTD_Sequence *inSeqs,
                                          size_t inSeqsSize,
                                          ZSTD_SequencePosition seqPos) {
  int end = 0;
  size_t blockSize = 0;
  size_t spos = seqPos.idx;
  DEBUGLOG(6, "blockSize_explicitDelimiter : seq %zu / %zu", spos, inSeqsSize);
  assert(spos <= inSeqsSize);
  while (spos < inSeqsSize) {
    end = (inSeqs[spos].offset == 0);
    blockSize += inSeqs[spos].litLength + inSeqs[spos].matchLength;
    if (end) {
      if (inSeqs[spos].matchLength != 0)
        RETURN_ERROR(externalSequences_invalid,
                     "delimiter format error : both matchlength and offset "
                     "must be == 0");
      break;
    }
    spos++;
  }
  if (!end)
    RETURN_ERROR(externalSequences_invalid,
                 "Reached end of sequences without finding a block delimiter");
  return blockSize;
}

static size_t determine_blockSize(ZSTD_SequenceFormat_e mode, size_t blockSize,
                                  size_t remaining, const ZSTD_Sequence *inSeqs,
                                  size_t inSeqsSize,
                                  ZSTD_SequencePosition seqPos) {
  DEBUGLOG(6, "determine_blockSize : remainingSize = %zu", remaining);
  if (mode == ZSTD_sf_noBlockDelimiters) {

    return MIN(remaining, blockSize);
  }
  assert(mode == ZSTD_sf_explicitBlockDelimiters);
  {
    size_t const explicitBlockSize =
        blockSize_explicitDelimiter(inSeqs, inSeqsSize, seqPos);
    FORWARD_IF_ERROR(
        explicitBlockSize,
        "Error while determining block size with explicit delimiters");
    if (explicitBlockSize > blockSize)
      RETURN_ERROR(externalSequences_invalid,
                   "sequences incorrectly define a too large block");
    if (explicitBlockSize > remaining)
      RETURN_ERROR(externalSequences_invalid,
                   "sequences define a frame longer than source");
    return explicitBlockSize;
  }
}

static size_t ZSTD_compressSequences_internal(ZSTD_CCtx *cctx, void *dst,
                                              size_t dstCapacity,
                                              const ZSTD_Sequence *inSeqs,
                                              size_t inSeqsSize,
                                              const void *src, size_t srcSize) {
  size_t cSize = 0;
  size_t remaining = srcSize;
  ZSTD_SequencePosition seqPos = {0, 0, 0};

  const BYTE *ip = (BYTE const *)src;
  BYTE *op = (BYTE *)dst;
  ZSTD_SequenceCopier_f const sequenceCopier =
      ZSTD_selectSequenceCopier(cctx->appliedParams.blockDelimiters);

  DEBUGLOG(4, "ZSTD_compressSequences_internal srcSize: %zu, inSeqsSize: %zu",
           srcSize, inSeqsSize);

  if (remaining == 0) {
    U32 const cBlockHeader24 = 1 + (((U32)bt_raw) << 1);
    RETURN_ERROR_IF(dstCapacity < 4, dstSize_tooSmall,
                    "No room for empty frame block header");
    MEM_writeLE32(op, cBlockHeader24);
    op += ZSTD_blockHeaderSize;
    dstCapacity -= ZSTD_blockHeaderSize;
    cSize += ZSTD_blockHeaderSize;
  }

  while (remaining) {
    size_t compressedSeqsSize;
    size_t cBlockSize;
    size_t blockSize = determine_blockSize(cctx->appliedParams.blockDelimiters,
                                           cctx->blockSizeMax, remaining,
                                           inSeqs, inSeqsSize, seqPos);
    U32 const lastBlock = (blockSize == remaining);
    FORWARD_IF_ERROR(blockSize, "Error while trying to determine block size");
    assert(blockSize <= remaining);
    ZSTD_resetSeqStore(&cctx->seqStore);

    blockSize = sequenceCopier(cctx, &seqPos, inSeqs, inSeqsSize, ip, blockSize,
                               cctx->appliedParams.searchForExternalRepcodes);
    FORWARD_IF_ERROR(blockSize, "Bad sequence copy");

    if (blockSize < MIN_CBLOCK_SIZE + ZSTD_blockHeaderSize + 1 + 1) {
      cBlockSize =
          ZSTD_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
      FORWARD_IF_ERROR(cBlockSize, "Nocompress block failed");
      DEBUGLOG(5, "Block too small (%zu): data remains uncompressed: cSize=%zu",
               blockSize, cBlockSize);
      cSize += cBlockSize;
      ip += blockSize;
      op += cBlockSize;
      remaining -= blockSize;
      dstCapacity -= cBlockSize;
      continue;
    }

    RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize, dstSize_tooSmall,
                    "not enough dstCapacity to write a new compressed block");
    compressedSeqsSize = ZSTD_entropyCompressSeqStore(
        &cctx->seqStore, &cctx->blockState.prevCBlock->entropy,
        &cctx->blockState.nextCBlock->entropy, &cctx->appliedParams,
        op + ZSTD_blockHeaderSize, dstCapacity - ZSTD_blockHeaderSize,
        blockSize, cctx->tmpWorkspace, cctx->tmpWkspSize, cctx->bmi2);
    FORWARD_IF_ERROR(compressedSeqsSize,
                     "Compressing sequences of block failed");
    DEBUGLOG(5, "Compressed sequences size: %zu", compressedSeqsSize);

    if (!cctx->isFirstBlock && ZSTD_maybeRLE(&cctx->seqStore) &&
        ZSTD_isRLE(ip, blockSize)) {

      compressedSeqsSize = 1;
    }

    if (compressedSeqsSize == 0) {

      cBlockSize =
          ZSTD_noCompressBlock(op, dstCapacity, ip, blockSize, lastBlock);
      FORWARD_IF_ERROR(cBlockSize, "ZSTD_noCompressBlock failed");
      DEBUGLOG(5, "Writing out nocompress block, size: %zu", cBlockSize);
    } else if (compressedSeqsSize == 1) {
      cBlockSize =
          ZSTD_rleCompressBlock(op, dstCapacity, *ip, blockSize, lastBlock);
      FORWARD_IF_ERROR(cBlockSize, "ZSTD_rleCompressBlock failed");
      DEBUGLOG(5, "Writing out RLE block, size: %zu", cBlockSize);
    } else {
      U32 cBlockHeader;

      ZSTD_blockState_confirmRepcodesAndEntropyTables(&cctx->blockState);
      if (cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
          FSE_repeat_valid)
        cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
            FSE_repeat_check;

      cBlockHeader = lastBlock + (((U32)bt_compressed) << 1) +
                     (U32)(compressedSeqsSize << 3);
      MEM_writeLE24(op, cBlockHeader);
      cBlockSize = ZSTD_blockHeaderSize + compressedSeqsSize;
      DEBUGLOG(5, "Writing out compressed block, size: %zu", cBlockSize);
    }

    cSize += cBlockSize;

    if (lastBlock) {
      break;
    } else {
      ip += blockSize;
      op += cBlockSize;
      remaining -= blockSize;
      dstCapacity -= cBlockSize;
      cctx->isFirstBlock = 0;
    }
    DEBUGLOG(5, "cSize running total: %zu (remaining dstCapacity=%zu)", cSize,
             dstCapacity);
  }

  DEBUGLOG(4, "cSize final total: %zu", cSize);
  return cSize;
}

size_t ZSTD_compressSequences(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
                              const ZSTD_Sequence *inSeqs, size_t inSeqsSize,
                              const void *src, size_t srcSize) {
  BYTE *op = (BYTE *)dst;
  size_t cSize = 0;

  DEBUGLOG(4, "ZSTD_compressSequences (nbSeqs=%zu,dstCapacity=%zu)", inSeqsSize,
           dstCapacity);
  assert(cctx != NULL);
  FORWARD_IF_ERROR(ZSTD_CCtx_init_compressStream2(cctx, ZSTD_e_end, srcSize),
                   "CCtx initialization failed");

  {
    size_t const frameHeaderSize = ZSTD_writeFrameHeader(
        op, dstCapacity, &cctx->appliedParams, srcSize, cctx->dictID);
    op += frameHeaderSize;
    assert(frameHeaderSize <= dstCapacity);
    dstCapacity -= frameHeaderSize;
    cSize += frameHeaderSize;
  }
  if (cctx->appliedParams.fParams.checksumFlag && srcSize) {
    XXH64_update(&cctx->xxhState, src, srcSize);
  }

  {
    size_t const cBlocksSize = ZSTD_compressSequences_internal(
        cctx, op, dstCapacity, inSeqs, inSeqsSize, src, srcSize);
    FORWARD_IF_ERROR(cBlocksSize, "Compressing blocks failed!");
    cSize += cBlocksSize;
    assert(cBlocksSize <= dstCapacity);
    dstCapacity -= cBlocksSize;
  }

  if (cctx->appliedParams.fParams.checksumFlag) {
    U32 const checksum = (U32)XXH64_digest(&cctx->xxhState);
    RETURN_ERROR_IF(dstCapacity < 4, dstSize_tooSmall, "no room for checksum");
    DEBUGLOG(4, "Write checksum : %08X", (unsigned)checksum);
    MEM_writeLE32((char *)dst + cSize, checksum);
    cSize += 4;
  }

  DEBUGLOG(4, "Final compressed size: %zu", cSize);
  return cSize;
}

#if defined(ZSTD_ARCH_X86_AVX2)

#include <immintrin.h>

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences) {

  const __m256i addition = _mm256_setr_epi32(ZSTD_REP_NUM, 0, -MINMATCH, 0,
                                             ZSTD_REP_NUM, 0, -MINMATCH, 0);

  const __m256i limit = _mm256_set1_epi32(65535);

  const __m256i mask = _mm256_setr_epi8(

      0, 1, 2, 3, 4, 5, 8, 9, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80,
      (BYTE)0x80, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80,

      16, 17, 18, 19, 20, 21, 24, 25, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80,
      (BYTE)0x80, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80, (BYTE)0x80);

#define PERM_LANE_0X_E8 0xE8

  size_t longLen = 0, i = 0;

  ZSTD_STATIC_ASSERT(sizeof(ZSTD_Sequence) == 16);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, offset) == 0);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) == 8);
  ZSTD_STATIC_ASSERT(sizeof(SeqDef) == 8);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, offBase) == 0);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, mlBase) == 6);

  for (; i + 1 < nbSequences; i += 2) {

    __m256i vin = _mm256_loadu_si256((const __m256i *)(const void *)&inSeqs[i]);

    __m256i vadd = _mm256_add_epi32(vin, addition);

    __m256i ll_cmp = _mm256_cmpgt_epi32(vadd, limit);
    int ll_res = _mm256_movemask_epi8(ll_cmp);

    __m256i vshf = _mm256_shuffle_epi8(vadd, mask);

    __m256i vperm = _mm256_permute4x64_epi64(vshf, PERM_LANE_0X_E8);

    _mm_storeu_si128((__m128i *)(void *)&dstSeqs[i],
                     _mm256_castsi256_si128(vperm));

    if (UNLIKELY((ll_res & 0x0FF00FF0) != 0)) {

      if (inSeqs[i].matchLength > 65535 + MINMATCH) {
        assert(longLen == 0);
        longLen = i + 1;
      }
      if (inSeqs[i].litLength > 65535) {
        assert(longLen == 0);
        longLen = i + nbSequences + 1;
      }
      if (inSeqs[i + 1].matchLength > 65535 + MINMATCH) {
        assert(longLen == 0);
        longLen = i + 1 + 1;
      }
      if (inSeqs[i + 1].litLength > 65535) {
        assert(longLen == 0);
        longLen = i + 1 + nbSequences + 1;
      }
    }
  }

  if (i < nbSequences) {

    assert(i == nbSequences - 1);
    dstSeqs[i].offBase = OFFSET_TO_OFFBASE(inSeqs[i].offset);
    dstSeqs[i].litLength = (U16)inSeqs[i].litLength;
    dstSeqs[i].mlBase = (U16)(inSeqs[i].matchLength - MINMATCH);

    if (UNLIKELY(inSeqs[i].matchLength > 65535 + MINMATCH)) {
      assert(longLen == 0);
      longLen = i + 1;
    }
    if (UNLIKELY(inSeqs[i].litLength > 65535)) {
      assert(longLen == 0);
      longLen = i + nbSequences + 1;
    }
  }

  return longLen;
}

#elif defined(ZSTD_ARCH_RISCV_RVV)
#include <riscv_vector.h>

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences) {
  size_t longLen = 0;
  size_t vl = 0;
  typedef uint32_t __attribute__((may_alias)) aliased_u32;

  ZSTD_STATIC_ASSERT(sizeof(ZSTD_Sequence) == 16);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, offset) == 0);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) == 8);
  ZSTD_STATIC_ASSERT(sizeof(SeqDef) == 8);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, offBase) == 0);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, mlBase) == 6);

  for (size_t i = 0; i < nbSequences; i += vl) {

    vl = __riscv_vsetvl_e32m2(nbSequences - i);
    {

      vuint32m2x4_t v_tuple = __riscv_vlseg4e32_v_u32m2x4(
          (const aliased_u32 *)((const void *)&inSeqs[i]), vl);
      vuint32m2_t v_offset = __riscv_vget_v_u32m2x4_u32m2(v_tuple, 0);
      vuint32m2_t v_lit = __riscv_vget_v_u32m2x4_u32m2(v_tuple, 1);
      vuint32m2_t v_match = __riscv_vget_v_u32m2x4_u32m2(v_tuple, 2);

      vuint32m2_t v_offBase = __riscv_vadd_vx_u32m2(v_offset, ZSTD_REP_NUM, vl);

      vbool16_t lit_overflow = __riscv_vmsgtu_vx_u32m2_b16(v_lit, 65535, vl);
      vuint16m1_t v_lit_clamped = __riscv_vncvt_x_x_w_u16m1(v_lit, vl);

      vbool16_t ml_overflow =
          __riscv_vmsgtu_vx_u32m2_b16(v_match, 65535 + MINMATCH, vl);
      vuint16m1_t v_ml_clamped = __riscv_vncvt_x_x_w_u16m1(
          __riscv_vsub_vx_u32m2(v_match, MINMATCH, vl), vl);

      vuint32m2_t v_lit_ml_combined = __riscv_vsll_vx_u32m2(
          __riscv_vwcvtu_x_x_v_u32m2(v_ml_clamped, vl), 16, vl);
      v_lit_ml_combined = __riscv_vor_vv_u32m2(
          v_lit_ml_combined, __riscv_vwcvtu_x_x_v_u32m2(v_lit_clamped, vl), vl);
      {

        vuint32m2x2_t store_data =
            __riscv_vcreate_v_u32m2x2(v_offBase, v_lit_ml_combined);
        __riscv_vsseg2e32_v_u32m2x2((aliased_u32 *)((void *)&dstSeqs[i]),
                                    store_data, vl);
      }
      {

        int first_ml = __riscv_vfirst_m_b16(ml_overflow, vl);
        int first_lit = __riscv_vfirst_m_b16(lit_overflow, vl);

        if (UNLIKELY(first_ml != -1)) {
          assert(longLen == 0);
          longLen = i + first_ml + 1;
        }
        if (UNLIKELY(first_lit != -1)) {
          assert(longLen == 0);
          longLen = i + first_lit + 1 + nbSequences;
        }
      }
    }
  }
  return longLen;
}

#elif defined(ZSTD_ARCH_ARM_SVE2)

FORCE_INLINE_TEMPLATE int cmpgtz_any_s8(svbool_t g, svint8_t a) {
  svbool_t ptest = svcmpgt_n_s8(g, a, 0);
  return svptest_any(ptest, ptest);
}

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences) {

  const size_t lanes = 8 * svcntb() / sizeof(ZSTD_Sequence);
  size_t longLen = 0;
  size_t n = 0;

  ZSTD_STATIC_ASSERT(sizeof(ZSTD_Sequence) == 16);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, offset) == 0);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) == 8);
  ZSTD_STATIC_ASSERT(sizeof(SeqDef) == 8);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, offBase) == 0);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, mlBase) == 6);

  if (nbSequences >= lanes) {
    const svbool_t ptrue = svptrue_b8();

    const svuint32_t vaddition =
        svreinterpret_u32(svunpklo_s32(svreinterpret_s16(
            svdup_n_u64(ZSTD_REP_NUM | (((U64)(U16)-MINMATCH) << 32)))));

    const svuint16_t vmask =
        svreinterpret_u16(svindex_u64(0x0004000200010000, 0x0008000800080008));

    const svbool_t pmid = svcmpne_n_u8(
        ptrue, svreinterpret_u8(svdup_n_u64(0x0000FFFFFFFF0000)), 0);

    do {

      const svuint32_t vin0 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 0);
      const svuint32_t vin1 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 1);
      const svuint32_t vin2 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 2);
      const svuint32_t vin3 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 3);
      const svuint32_t vin4 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 4);
      const svuint32_t vin5 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 5);
      const svuint32_t vin6 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 6);
      const svuint32_t vin7 = svld1_vnum_u32(ptrue, &inSeqs[n].offset, 7);

      const svuint16x2_t vadd01 =
          svcreate2_u16(svreinterpret_u16(svadd_u32_x(ptrue, vin0, vaddition)),
                        svreinterpret_u16(svadd_u32_x(ptrue, vin1, vaddition)));
      const svuint16x2_t vadd23 =
          svcreate2_u16(svreinterpret_u16(svadd_u32_x(ptrue, vin2, vaddition)),
                        svreinterpret_u16(svadd_u32_x(ptrue, vin3, vaddition)));
      const svuint16x2_t vadd45 =
          svcreate2_u16(svreinterpret_u16(svadd_u32_x(ptrue, vin4, vaddition)),
                        svreinterpret_u16(svadd_u32_x(ptrue, vin5, vaddition)));
      const svuint16x2_t vadd67 =
          svcreate2_u16(svreinterpret_u16(svadd_u32_x(ptrue, vin6, vaddition)),
                        svreinterpret_u16(svadd_u32_x(ptrue, vin7, vaddition)));

      const svuint16_t vout01 = svtbl2_u16(vadd01, vmask);
      const svuint16_t vout23 = svtbl2_u16(vadd23, vmask);
      const svuint16_t vout45 = svtbl2_u16(vadd45, vmask);
      const svuint16_t vout67 = svtbl2_u16(vadd67, vmask);

      const svuint16_t voverflow01 =
          svuzp2_u16(svget2_u16(vadd01, 0), svget2_u16(vadd01, 1));
      const svuint16_t voverflow23 =
          svuzp2_u16(svget2_u16(vadd23, 0), svget2_u16(vadd23, 1));
      const svuint16_t voverflow45 =
          svuzp2_u16(svget2_u16(vadd45, 0), svget2_u16(vadd45, 1));
      const svuint16_t voverflow67 =
          svuzp2_u16(svget2_u16(vadd67, 0), svget2_u16(vadd67, 1));

      const svint8_t voverflow =
          svmax_s8_x(pmid,
                     svtrn1_s8(svreinterpret_s8(voverflow01),
                               svreinterpret_s8(voverflow23)),
                     svtrn1_s8(svreinterpret_s8(voverflow45),
                               svreinterpret_s8(voverflow67)));

      svst1_vnum_u32(ptrue, &dstSeqs[n].offBase, 0, svreinterpret_u32(vout01));
      svst1_vnum_u32(ptrue, &dstSeqs[n].offBase, 1, svreinterpret_u32(vout23));
      svst1_vnum_u32(ptrue, &dstSeqs[n].offBase, 2, svreinterpret_u32(vout45));
      svst1_vnum_u32(ptrue, &dstSeqs[n].offBase, 3, svreinterpret_u32(vout67));

      if (UNLIKELY(cmpgtz_any_s8(pmid, voverflow))) {

        size_t i;
        for (i = n; i < n + lanes; i++) {
          if (inSeqs[i].matchLength > 65535 + MINMATCH) {
            assert(longLen == 0);
            longLen = i + 1;
          }
          if (inSeqs[i].litLength > 65535) {
            assert(longLen == 0);
            longLen = i + nbSequences + 1;
          }
        }
      }

      n += lanes;
    } while (n <= nbSequences - lanes);
  }

  for (; n < nbSequences; n++) {
    dstSeqs[n].offBase = OFFSET_TO_OFFBASE(inSeqs[n].offset);
    dstSeqs[n].litLength = (U16)inSeqs[n].litLength;
    dstSeqs[n].mlBase = (U16)(inSeqs[n].matchLength - MINMATCH);

    if (UNLIKELY(inSeqs[n].matchLength > 65535 + MINMATCH)) {
      assert(longLen == 0);
      longLen = n + 1;
    }
    if (UNLIKELY(inSeqs[n].litLength > 65535)) {
      assert(longLen == 0);
      longLen = n + nbSequences + 1;
    }
  }
  return longLen;
}

#elif defined(ZSTD_ARCH_ARM_NEON) && (defined(__aarch64__) || defined(_M_ARM64))

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences) {
  size_t longLen = 0;
  size_t n = 0;

  ZSTD_STATIC_ASSERT(sizeof(ZSTD_Sequence) == 16);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, offset) == 0);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) == 8);
  ZSTD_STATIC_ASSERT(sizeof(SeqDef) == 8);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, offBase) == 0);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, litLength) == 4);
  ZSTD_STATIC_ASSERT(offsetof(SeqDef, mlBase) == 6);

  if (nbSequences > 3) {
    static const ZSTD_ALIGNED(16)
        U32 constAddition[4] = {ZSTD_REP_NUM, 0, -MINMATCH, 0};
    static const ZSTD_ALIGNED(16) U8 constMask[16] = {
        0, 1, 2, 3, 4, 5, 8, 9, 16, 17, 18, 19, 20, 21, 24, 25};
    static const ZSTD_ALIGNED(16)
        U16 constCounter[8] = {1, 1, 1, 1, 2, 2, 2, 2};

    const uint32x4_t vaddition = vld1q_u32(constAddition);
    const uint8x16_t vmask = vld1q_u8(constMask);
    uint16x8_t vcounter = vld1q_u16(constCounter);
    uint16x8_t vindex01 = vdupq_n_u16(0);
    uint16x8_t vindex23 = vdupq_n_u16(0);

    do {

      const uint32x4_t vin0 = vld1q_u32(&inSeqs[n + 0].offset);
      const uint32x4_t vin1 = vld1q_u32(&inSeqs[n + 1].offset);
      const uint32x4_t vin2 = vld1q_u32(&inSeqs[n + 2].offset);
      const uint32x4_t vin3 = vld1q_u32(&inSeqs[n + 3].offset);

      const uint8x16x2_t vadd01 = {{
          vreinterpretq_u8_u32(vaddq_u32(vin0, vaddition)),
          vreinterpretq_u8_u32(vaddq_u32(vin1, vaddition)),
      }};
      const uint8x16x2_t vadd23 = {{
          vreinterpretq_u8_u32(vaddq_u32(vin2, vaddition)),
          vreinterpretq_u8_u32(vaddq_u32(vin3, vaddition)),
      }};

      const uint8x16_t vout01 = vqtbl2q_u8(vadd01, vmask);
      const uint8x16_t vout23 = vqtbl2q_u8(vadd23, vmask);

      uint16x8_t voverflow01 = vuzp2q_u16(vreinterpretq_u16_u8(vadd01.val[0]),
                                          vreinterpretq_u16_u8(vadd01.val[1]));
      uint16x8_t voverflow23 = vuzp2q_u16(vreinterpretq_u16_u8(vadd23.val[0]),
                                          vreinterpretq_u16_u8(vadd23.val[1]));

      vst1q_u32(&dstSeqs[n + 0].offBase, vreinterpretq_u32_u8(vout01));
      vst1q_u32(&dstSeqs[n + 2].offBase, vreinterpretq_u32_u8(vout23));

      voverflow01 = vcgtzq_s16(vreinterpretq_s16_u16(voverflow01));
      voverflow23 = vcgtzq_s16(vreinterpretq_s16_u16(voverflow23));

      vindex01 = vbslq_u16(voverflow01, vcounter, vindex01);
      vindex23 = vbslq_u16(voverflow23, vcounter, vindex23);

      vcounter = vaddq_u16(vcounter, vdupq_n_u16(4));

      n += 4;
    } while (n < nbSequences - 3);

    {
      uint16x8_t nonzero = vtstq_u16(vindex23, vindex23);
      vindex23 = vsubq_u16(vindex23, nonzero);
      vindex23 = vsubq_u16(vindex23, nonzero);
    }

    vindex01 = vmaxq_u16(vindex01, vindex23);
    vindex01 = vmaxq_u16(vindex01, vextq_u16(vindex01, vindex01, 4));

    {
      U64 maxLitMatchIndices =
          vgetq_lane_u64(vreinterpretq_u64_u16(vindex01), 0);
      size_t maxLitIndex = (maxLitMatchIndices >> 16) & 0xFFFF;
      size_t maxMatchIndex = (maxLitMatchIndices >> 32) & 0xFFFF;
      longLen = maxLitIndex > maxMatchIndex ? maxLitIndex + nbSequences
                                            : maxMatchIndex;
    }
  }

  for (; n < nbSequences; n++) {
    dstSeqs[n].offBase = OFFSET_TO_OFFBASE(inSeqs[n].offset);
    dstSeqs[n].litLength = (U16)inSeqs[n].litLength;
    dstSeqs[n].mlBase = (U16)(inSeqs[n].matchLength - MINMATCH);

    if (UNLIKELY(inSeqs[n].matchLength > 65535 + MINMATCH)) {
      assert(longLen == 0);
      longLen = n + 1;
    }
    if (UNLIKELY(inSeqs[n].litLength > 65535)) {
      assert(longLen == 0);
      longLen = n + nbSequences + 1;
    }
  }
  return longLen;
}

#else

size_t convertSequences_noRepcodes(SeqDef *dstSeqs, const ZSTD_Sequence *inSeqs,
                                   size_t nbSequences) {
  size_t longLen = 0;
  size_t n;
  for (n = 0; n < nbSequences; n++) {
    dstSeqs[n].offBase = OFFSET_TO_OFFBASE(inSeqs[n].offset);
    dstSeqs[n].litLength = (U16)inSeqs[n].litLength;
    dstSeqs[n].mlBase = (U16)(inSeqs[n].matchLength - MINMATCH);

    if (UNLIKELY(inSeqs[n].matchLength > 65535 + MINMATCH)) {
      assert(longLen == 0);
      longLen = n + 1;
    }
    if (UNLIKELY(inSeqs[n].litLength > 65535)) {
      assert(longLen == 0);
      longLen = n + nbSequences + 1;
    }
  }
  return longLen;
}

#endif

size_t ZSTD_convertBlockSequences(ZSTD_CCtx *cctx, const ZSTD_Sequence *inSeqs,
                                  size_t nbSequences, int repcodeResolution) {
  Repcodes_t updatedRepcodes;
  size_t seqNb = 0;

  DEBUGLOG(5, "ZSTD_convertBlockSequences (nbSequences = %zu)", nbSequences);

  RETURN_ERROR_IF(
      nbSequences >= cctx->seqStore.maxNbSeq, externalSequences_invalid,
      "Not enough memory allocated. Try adjusting ZSTD_c_minMatch.");

  ZSTD_memcpy(updatedRepcodes.rep, cctx->blockState.prevCBlock->rep,
              sizeof(Repcodes_t));

  assert(nbSequences >= 1);
  assert(inSeqs[nbSequences - 1].matchLength == 0);
  assert(inSeqs[nbSequences - 1].offset == 0);

  if (!repcodeResolution) {
    size_t const longl = convertSequences_noRepcodes(
        cctx->seqStore.sequencesStart, inSeqs, nbSequences - 1);
    cctx->seqStore.sequences = cctx->seqStore.sequencesStart + nbSequences - 1;
    if (longl) {
      DEBUGLOG(5, "long length");
      assert(cctx->seqStore.longLengthType == ZSTD_llt_none);
      if (longl <= nbSequences - 1) {
        DEBUGLOG(5, "long match length detected at pos %zu", longl - 1);
        cctx->seqStore.longLengthType = ZSTD_llt_matchLength;
        cctx->seqStore.longLengthPos = (U32)(longl - 1);
      } else {
        DEBUGLOG(5, "long literals length detected at pos %zu",
                 longl - nbSequences);
        assert(longl <= 2 * (nbSequences - 1));
        cctx->seqStore.longLengthType = ZSTD_llt_literalLength;
        cctx->seqStore.longLengthPos = (U32)(longl - (nbSequences - 1) - 1);
      }
    }
  } else {
    for (seqNb = 0; seqNb < nbSequences - 1; seqNb++) {
      U32 const litLength = inSeqs[seqNb].litLength;
      U32 const matchLength = inSeqs[seqNb].matchLength;
      U32 const ll0 = (litLength == 0);
      U32 const offBase =
          ZSTD_finalizeOffBase(inSeqs[seqNb].offset, updatedRepcodes.rep, ll0);

      DEBUGLOG(6, "Storing sequence: (of: %u, ml: %u, ll: %u)", offBase,
               matchLength, litLength);
      ZSTD_storeSeqOnly(&cctx->seqStore, litLength, offBase, matchLength);
      ZSTD_updateRep(updatedRepcodes.rep, offBase, ll0);
    }
  }

  if (!repcodeResolution && nbSequences > 1) {
    U32 *const rep = updatedRepcodes.rep;

    if (nbSequences >= 4) {
      U32 lastSeqIdx = (U32)nbSequences - 2;
      rep[2] = inSeqs[lastSeqIdx - 2].offset;
      rep[1] = inSeqs[lastSeqIdx - 1].offset;
      rep[0] = inSeqs[lastSeqIdx].offset;
    } else if (nbSequences == 3) {
      rep[2] = rep[0];
      rep[1] = inSeqs[0].offset;
      rep[0] = inSeqs[1].offset;
    } else {
      assert(nbSequences == 2);
      rep[2] = rep[1];
      rep[1] = rep[0];
      rep[0] = inSeqs[0].offset;
    }
  }

  ZSTD_memcpy(cctx->blockState.nextCBlock->rep, updatedRepcodes.rep,
              sizeof(Repcodes_t));

  return 0;
}

#if defined(ZSTD_ARCH_X86_AVX2)

BlockSummary ZSTD_get1BlockSummary(const ZSTD_Sequence *seqs, size_t nbSeqs) {
  size_t i;
  __m256i const zeroVec = _mm256_setzero_si256();
  __m256i sumVec = zeroVec;
  ZSTD_ALIGNED(32) U32 tmp[8];
  size_t mSum = 0, lSum = 0;
  ZSTD_STATIC_ASSERT(sizeof(ZSTD_Sequence) == 16);

  for (i = 0; i + 2 <= nbSeqs; i += 2) {

    __m256i data = _mm256_loadu_si256((const __m256i *)(const void *)&seqs[i]);

    __m256i cmp = _mm256_cmpeq_epi32(data, zeroVec);
    int cmp_res = _mm256_movemask_epi8(cmp);

    ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) == 8);
    if (cmp_res & 0x0F000F00)
      break;

    sumVec = _mm256_add_epi32(sumVec, data);
  }

  _mm256_store_si256((__m256i *)tmp, sumVec);
  lSum = tmp[1] + tmp[5];
  mSum = tmp[2] + tmp[6];

  for (; i < nbSeqs; i++) {
    lSum += seqs[i].litLength;
    mSum += seqs[i].matchLength;
    if (seqs[i].matchLength == 0)
      break;
  }

  if (i == nbSeqs) {

    BlockSummary bs;
    bs.nbSequences = ERROR(externalSequences_invalid);
    return bs;
  }
  {
    BlockSummary bs;
    bs.nbSequences = i + 1;
    bs.blockSize = lSum + mSum;
    bs.litSize = lSum;
    return bs;
  }
}

#elif defined(ZSTD_ARCH_RISCV_RVV)

BlockSummary ZSTD_get1BlockSummary(const ZSTD_Sequence *seqs, size_t nbSeqs) {
  size_t totalMatchSize = 0;
  size_t litSize = 0;
  size_t i = 0;
  int found_terminator = 0;
  size_t vl_max = __riscv_vsetvlmax_e32m1();
  typedef uint32_t __attribute__((may_alias)) aliased_u32;
  vuint32m1_t v_lit_sum = __riscv_vmv_v_x_u32m1(0, vl_max);
  vuint32m1_t v_match_sum = __riscv_vmv_v_x_u32m1(0, vl_max);

  for (; i < nbSeqs;) {
    size_t vl = __riscv_vsetvl_e32m2(nbSeqs - i);

    vuint32m2x4_t v_tuple = __riscv_vlseg4e32_v_u32m2x4(
        (const aliased_u32 *)((const void *)&seqs[i]), vl);
    vuint32m2_t v_lit = __riscv_vget_v_u32m2x4_u32m2(v_tuple, 1);
    vuint32m2_t v_match = __riscv_vget_v_u32m2x4_u32m2(v_tuple, 2);

    vbool16_t mask = __riscv_vmseq_vx_u32m2_b16(v_match, 0, vl);
    int first_zero = __riscv_vfirst_m_b16(mask, vl);

    if (first_zero >= 0) {

      vl = first_zero + 1;

      v_lit_sum = __riscv_vredsum_vs_u32m2_u32m1(
          __riscv_vslidedown_vx_u32m2(v_lit, 0, vl), v_lit_sum, vl);
      v_match_sum = __riscv_vredsum_vs_u32m2_u32m1(
          __riscv_vslidedown_vx_u32m2(v_match, 0, vl), v_match_sum, vl);

      i += vl;
      found_terminator = 1;
      assert(seqs[i - 1].offset == 0);
      break;
    } else {

      v_lit_sum = __riscv_vredsum_vs_u32m2_u32m1(v_lit, v_lit_sum, vl);
      v_match_sum = __riscv_vredsum_vs_u32m2_u32m1(v_match, v_match_sum, vl);
      i += vl;
    }
  }
  litSize = __riscv_vmv_x_s_u32m1_u32(v_lit_sum);
  totalMatchSize = __riscv_vmv_x_s_u32m1_u32(v_match_sum);

  if (!found_terminator && i == nbSeqs) {
    BlockSummary bs;
    bs.nbSequences = ERROR(externalSequences_invalid);
    return bs;
  }
  {
    BlockSummary bs;
    bs.nbSequences = i;
    bs.blockSize = litSize + totalMatchSize;
    bs.litSize = litSize;
    return bs;
  }
}

#else

FORCE_INLINE_TEMPLATE int matchLengthHalfIsZero(U64 litMatchLength) {
  if (MEM_isLittleEndian()) {
    return litMatchLength <= 0xFFFFFFFFULL;
  } else {
    return (U32)litMatchLength == 0;
  }
}

BlockSummary ZSTD_get1BlockSummary(const ZSTD_Sequence *seqs, size_t nbSeqs) {

  U64 litMatchSize0 = 0;
  U64 litMatchSize1 = 0;
  U64 litMatchSize2 = 0;
  U64 litMatchSize3 = 0;
  size_t n = 0;

  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, litLength) + 4 ==
                     offsetof(ZSTD_Sequence, matchLength));
  ZSTD_STATIC_ASSERT(offsetof(ZSTD_Sequence, matchLength) + 4 ==
                     offsetof(ZSTD_Sequence, rep));
  assert(seqs);

  if (nbSeqs > 3) {

    do {

      U64 litMatchLength = MEM_read64(&seqs[n].litLength);
      litMatchSize0 += litMatchLength;
      if (matchLengthHalfIsZero(litMatchLength)) {
        assert(seqs[n].offset == 0);
        goto _out;
      }

      litMatchLength = MEM_read64(&seqs[n + 1].litLength);
      litMatchSize1 += litMatchLength;
      if (matchLengthHalfIsZero(litMatchLength)) {
        n += 1;
        assert(seqs[n].offset == 0);
        goto _out;
      }

      litMatchLength = MEM_read64(&seqs[n + 2].litLength);
      litMatchSize2 += litMatchLength;
      if (matchLengthHalfIsZero(litMatchLength)) {
        n += 2;
        assert(seqs[n].offset == 0);
        goto _out;
      }

      litMatchLength = MEM_read64(&seqs[n + 3].litLength);
      litMatchSize3 += litMatchLength;
      if (matchLengthHalfIsZero(litMatchLength)) {
        n += 3;
        assert(seqs[n].offset == 0);
        goto _out;
      }

      n += 4;
    } while (n < nbSeqs - 3);
  }

  for (; n < nbSeqs; n++) {
    U64 litMatchLength = MEM_read64(&seqs[n].litLength);
    litMatchSize0 += litMatchLength;
    if (matchLengthHalfIsZero(litMatchLength)) {
      assert(seqs[n].offset == 0);
      goto _out;
    }
  }

  {
    BlockSummary bs;
    memset(&bs, 0, sizeof(bs));
    bs.nbSequences = ERROR(externalSequences_invalid);
    return bs;
  }
_out:
  litMatchSize0 += litMatchSize1 + litMatchSize2 + litMatchSize3;
  {
    BlockSummary bs;
    bs.nbSequences = n + 1;
    if (MEM_isLittleEndian()) {
      bs.litSize = (U32)litMatchSize0;
      bs.blockSize = bs.litSize + (litMatchSize0 >> 32);
    } else {
      bs.litSize = litMatchSize0 >> 32;
      bs.blockSize = bs.litSize + (U32)litMatchSize0;
    }
    return bs;
  }
}
#endif

static size_t ZSTD_compressSequencesAndLiterals_internal(
    ZSTD_CCtx *cctx, void *dst, size_t dstCapacity, const ZSTD_Sequence *inSeqs,
    size_t nbSequences, const void *literals, size_t litSize, size_t srcSize) {
  size_t remaining = srcSize;
  size_t cSize = 0;
  BYTE *op = (BYTE *)dst;
  int const repcodeResolution =
      (cctx->appliedParams.searchForExternalRepcodes == ZSTD_ps_enable);
  assert(cctx->appliedParams.searchForExternalRepcodes != ZSTD_ps_auto);

  DEBUGLOG(
      4, "ZSTD_compressSequencesAndLiterals_internal: nbSeqs=%zu, litSize=%zu",
      nbSequences, litSize);
  RETURN_ERROR_IF(nbSequences == 0, externalSequences_invalid,
                  "Requires at least 1 end-of-block");

  if ((nbSequences == 1) && (inSeqs[0].litLength == 0)) {
    U32 const cBlockHeader24 = 1 + (((U32)bt_raw) << 1);
    RETURN_ERROR_IF(dstCapacity < 3, dstSize_tooSmall,
                    "No room for empty frame block header");
    MEM_writeLE24(op, cBlockHeader24);
    op += ZSTD_blockHeaderSize;
    dstCapacity -= ZSTD_blockHeaderSize;
    cSize += ZSTD_blockHeaderSize;
  }

  while (nbSequences) {
    size_t compressedSeqsSize, cBlockSize = 0, conversionStatus;
    BlockSummary const block = ZSTD_get1BlockSummary(inSeqs, nbSequences);
    U32 const lastBlock = (block.nbSequences == nbSequences);
    FORWARD_IF_ERROR(
        block.nbSequences,
        "Error while trying to determine nb of sequences for a block");
    assert(block.nbSequences <= nbSequences);
    RETURN_ERROR_IF(
        block.litSize > litSize, externalSequences_invalid,
        "discrepancy: Sequences require more literals than present in buffer");
    ZSTD_resetSeqStore(&cctx->seqStore);

    conversionStatus = ZSTD_convertBlockSequences(
        cctx, inSeqs, block.nbSequences, repcodeResolution);
    FORWARD_IF_ERROR(conversionStatus, "Bad sequence conversion");
    inSeqs += block.nbSequences;
    nbSequences -= block.nbSequences;
    remaining -= block.blockSize;

    RETURN_ERROR_IF(dstCapacity < ZSTD_blockHeaderSize, dstSize_tooSmall,
                    "not enough dstCapacity to write a new compressed block");

    compressedSeqsSize = ZSTD_entropyCompressSeqStore_internal(
        op + ZSTD_blockHeaderSize, dstCapacity - ZSTD_blockHeaderSize, literals,
        block.litSize, &cctx->seqStore, &cctx->blockState.prevCBlock->entropy,
        &cctx->blockState.nextCBlock->entropy, &cctx->appliedParams,
        cctx->tmpWorkspace, cctx->tmpWkspSize, cctx->bmi2);
    FORWARD_IF_ERROR(compressedSeqsSize,
                     "Compressing sequences of block failed");

    if (compressedSeqsSize > cctx->blockSizeMax)
      compressedSeqsSize = 0;
    DEBUGLOG(5, "Compressed sequences size: %zu", compressedSeqsSize);
    litSize -= block.litSize;
    literals = (const char *)literals + block.litSize;

    if (compressedSeqsSize == 0) {

      RETURN_ERROR(cannotProduce_uncompressedBlock,
                   "ZSTD_compressSequencesAndLiterals cannot generate an "
                   "uncompressed block");
    } else {
      U32 cBlockHeader;
      assert(compressedSeqsSize > 1);

      ZSTD_blockState_confirmRepcodesAndEntropyTables(&cctx->blockState);
      if (cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode ==
          FSE_repeat_valid)
        cctx->blockState.prevCBlock->entropy.fse.offcode_repeatMode =
            FSE_repeat_check;

      cBlockHeader = lastBlock + (((U32)bt_compressed) << 1) +
                     (U32)(compressedSeqsSize << 3);
      MEM_writeLE24(op, cBlockHeader);
      cBlockSize = ZSTD_blockHeaderSize + compressedSeqsSize;
      DEBUGLOG(5, "Writing out compressed block, size: %zu", cBlockSize);
    }

    cSize += cBlockSize;
    op += cBlockSize;
    dstCapacity -= cBlockSize;
    cctx->isFirstBlock = 0;
    DEBUGLOG(5, "cSize running total: %zu (remaining dstCapacity=%zu)", cSize,
             dstCapacity);

    if (lastBlock) {
      assert(nbSequences == 0);
      break;
    }
  }

  RETURN_ERROR_IF(litSize != 0, externalSequences_invalid,
                  "literals must be entirely and exactly consumed");
  RETURN_ERROR_IF(remaining != 0, externalSequences_invalid,
                  "Sequences must represent a total of exactly srcSize=%zu",
                  srcSize);
  DEBUGLOG(4, "cSize final total: %zu", cSize);
  return cSize;
}

size_t ZSTD_compressSequencesAndLiterals(
    ZSTD_CCtx *cctx, void *dst, size_t dstCapacity, const ZSTD_Sequence *inSeqs,
    size_t inSeqsSize, const void *literals, size_t litSize, size_t litCapacity,
    size_t decompressedSize) {
  BYTE *op = (BYTE *)dst;
  size_t cSize = 0;

  DEBUGLOG(4, "ZSTD_compressSequencesAndLiterals (dstCapacity=%zu)",
           dstCapacity);
  assert(cctx != NULL);
  if (litCapacity < litSize) {
    RETURN_ERROR(workSpace_tooSmall,
                 "literals buffer is not large enough: must be at least 8 "
                 "bytes larger than litSize (risk of read out-of-bound)");
  }
  FORWARD_IF_ERROR(
      ZSTD_CCtx_init_compressStream2(cctx, ZSTD_e_end, decompressedSize),
      "CCtx initialization failed");

  if (cctx->appliedParams.blockDelimiters == ZSTD_sf_noBlockDelimiters) {
    RETURN_ERROR(frameParameter_unsupported,
                 "This mode is only compatible with explicit delimiters");
  }
  if (cctx->appliedParams.validateSequences) {
    RETURN_ERROR(parameter_unsupported,
                 "This mode is not compatible with Sequence validation");
  }
  if (cctx->appliedParams.fParams.checksumFlag) {
    RETURN_ERROR(frameParameter_unsupported,
                 "this mode is not compatible with frame checksum");
  }

  {
    size_t const frameHeaderSize = ZSTD_writeFrameHeader(
        op, dstCapacity, &cctx->appliedParams, decompressedSize, cctx->dictID);
    op += frameHeaderSize;
    assert(frameHeaderSize <= dstCapacity);
    dstCapacity -= frameHeaderSize;
    cSize += frameHeaderSize;
  }

  {
    size_t const cBlocksSize = ZSTD_compressSequencesAndLiterals_internal(
        cctx, op, dstCapacity, inSeqs, inSeqsSize, literals, litSize,
        decompressedSize);
    FORWARD_IF_ERROR(cBlocksSize, "Compressing blocks failed!");
    cSize += cBlocksSize;
    assert(cBlocksSize <= dstCapacity);
    dstCapacity -= cBlocksSize;
  }

  DEBUGLOG(4, "Final compressed size: %zu", cSize);
  return cSize;
}

static ZSTD_inBuffer inBuffer_forEndFlush(const ZSTD_CStream *zcs) {
  const ZSTD_inBuffer nullInput = {NULL, 0, 0};
  const int stableInput = (zcs->appliedParams.inBufferMode == ZSTD_bm_stable);
  return stableInput ? zcs->expectedInBuffer : nullInput;
}

size_t ZSTD_flushStream(ZSTD_CStream *zcs, ZSTD_outBuffer *output) {
  ZSTD_inBuffer input = inBuffer_forEndFlush(zcs);
  input.size = input.pos;
  return ZSTD_compressStream2(zcs, output, &input, ZSTD_e_flush);
}

size_t ZSTD_endStream(ZSTD_CStream *zcs, ZSTD_outBuffer *output) {
  ZSTD_inBuffer input = inBuffer_forEndFlush(zcs);
  size_t const remainingToFlush =
      ZSTD_compressStream2(zcs, output, &input, ZSTD_e_end);
  FORWARD_IF_ERROR(remainingToFlush,
                   "ZSTD_compressStream2(,,ZSTD_e_end) failed");
  if (zcs->appliedParams.nbWorkers > 0)
    return remainingToFlush;

  {
    size_t const lastBlockSize = zcs->frameEnded ? 0 : ZSTD_BLOCKHEADERSIZE;
    size_t const checksumSize =
        (size_t)(zcs->frameEnded ? 0
                                 : zcs->appliedParams.fParams.checksumFlag * 4);
    size_t const toFlush = remainingToFlush + lastBlockSize + checksumSize;
    DEBUGLOG(4, "ZSTD_endStream : remaining to flush : %u", (unsigned)toFlush);
    return toFlush;
  }
}

#include "clevels.h"

int ZSTD_maxCLevel(void) { return ZSTD_MAX_CLEVEL; }
int ZSTD_minCLevel(void) { return (int)-ZSTD_TARGETLENGTH_MAX; }
int ZSTD_defaultCLevel(void) { return ZSTD_CLEVEL_DEFAULT; }

static ZSTD_compressionParameters
ZSTD_dedicatedDictSearch_getCParams(int const compressionLevel,
                                    size_t const dictSize) {
  ZSTD_compressionParameters cParams = ZSTD_getCParams_internal(
      compressionLevel, 0, dictSize, ZSTD_cpm_createCDict);
  switch (cParams.strategy) {
  case ZSTD_fast:
  case ZSTD_dfast:
    break;
  case ZSTD_greedy:
  case ZSTD_lazy:
  case ZSTD_lazy2:
    cParams.hashLog += ZSTD_LAZY_DDSS_BUCKET_LOG;
    break;
  case ZSTD_btlazy2:
  case ZSTD_btopt:
  case ZSTD_btultra:
  case ZSTD_btultra2:
    break;
  }
  return cParams;
}

static int ZSTD_dedicatedDictSearch_isSupported(
    ZSTD_compressionParameters const *cParams) {
  return (cParams->strategy >= ZSTD_greedy) &&
         (cParams->strategy <= ZSTD_lazy2) &&
         (cParams->hashLog > cParams->chainLog) && (cParams->chainLog <= 24);
}

static void
ZSTD_dedicatedDictSearch_revertCParams(ZSTD_compressionParameters *cParams) {
  switch (cParams->strategy) {
  case ZSTD_fast:
  case ZSTD_dfast:
    break;
  case ZSTD_greedy:
  case ZSTD_lazy:
  case ZSTD_lazy2:
    cParams->hashLog -= ZSTD_LAZY_DDSS_BUCKET_LOG;
    if (cParams->hashLog < ZSTD_HASHLOG_MIN) {
      cParams->hashLog = ZSTD_HASHLOG_MIN;
    }
    break;
  case ZSTD_btlazy2:
  case ZSTD_btopt:
  case ZSTD_btultra:
  case ZSTD_btultra2:
    break;
  }
}

static U64 ZSTD_getCParamRowSize(U64 srcSizeHint, size_t dictSize,
                                 ZSTD_CParamMode_e mode) {
  switch (mode) {
  case ZSTD_cpm_unknown:
  case ZSTD_cpm_noAttachDict:
  case ZSTD_cpm_createCDict:
    break;
  case ZSTD_cpm_attachDict:
    dictSize = 0;
    break;
  default:
    assert(0);
    break;
  }
  {
    int const unknown = srcSizeHint == ZSTD_CONTENTSIZE_UNKNOWN;
    size_t const addedSize = unknown && dictSize > 0 ? 500 : 0;
    return unknown && dictSize == 0 ? ZSTD_CONTENTSIZE_UNKNOWN
                                    : srcSizeHint + dictSize + addedSize;
  }
}

static ZSTD_compressionParameters
ZSTD_getCParams_internal(int compressionLevel, unsigned long long srcSizeHint,
                         size_t dictSize, ZSTD_CParamMode_e mode) {
  U64 const rSize = ZSTD_getCParamRowSize(srcSizeHint, dictSize, mode);
  U32 const tableID = (rSize <= 256 KB) + (rSize <= 128 KB) + (rSize <= 16 KB);
  int row;
  DEBUGLOG(5, "ZSTD_getCParams_internal (cLevel=%i)", compressionLevel);

  if (compressionLevel == 0)
    row = ZSTD_CLEVEL_DEFAULT;
  else if (compressionLevel < 0)
    row = 0;
  else if (compressionLevel > ZSTD_MAX_CLEVEL)
    row = ZSTD_MAX_CLEVEL;
  else
    row = compressionLevel;

  {
    ZSTD_compressionParameters cp = ZSTD_defaultCParameters[tableID][row];
    DEBUGLOG(5,
             "ZSTD_getCParams_internal selected tableID: %u row: %u strat: %u",
             tableID, row, (U32)cp.strategy);

    if (compressionLevel < 0) {
      int const clampedCompressionLevel =
          MAX(ZSTD_minCLevel(), compressionLevel);
      cp.targetLength = (unsigned)(-clampedCompressionLevel);
    }

    return ZSTD_adjustCParams_internal(cp, srcSizeHint, dictSize, mode,
                                       ZSTD_ps_auto);
  }
}

ZSTD_compressionParameters ZSTD_getCParams(int compressionLevel,
                                           unsigned long long srcSizeHint,
                                           size_t dictSize) {
  if (srcSizeHint == 0)
    srcSizeHint = ZSTD_CONTENTSIZE_UNKNOWN;
  return ZSTD_getCParams_internal(compressionLevel, srcSizeHint, dictSize,
                                  ZSTD_cpm_unknown);
}

static ZSTD_parameters ZSTD_getParams_internal(int compressionLevel,
                                               unsigned long long srcSizeHint,
                                               size_t dictSize,
                                               ZSTD_CParamMode_e mode) {
  ZSTD_parameters params;
  ZSTD_compressionParameters const cParams =
      ZSTD_getCParams_internal(compressionLevel, srcSizeHint, dictSize, mode);
  DEBUGLOG(5, "ZSTD_getParams (cLevel=%i)", compressionLevel);
  ZSTD_memset(&params, 0, sizeof(params));
  params.cParams = cParams;
  params.fParams.contentSizeFlag = 1;
  return params;
}

ZSTD_parameters ZSTD_getParams(int compressionLevel,
                               unsigned long long srcSizeHint,
                               size_t dictSize) {
  if (srcSizeHint == 0)
    srcSizeHint = ZSTD_CONTENTSIZE_UNKNOWN;
  return ZSTD_getParams_internal(compressionLevel, srcSizeHint, dictSize,
                                 ZSTD_cpm_unknown);
}

void ZSTD_registerSequenceProducer(ZSTD_CCtx *zc, void *extSeqProdState,
                                   ZSTD_sequenceProducer_F extSeqProdFunc) {
  assert(zc != NULL);
  ZSTD_CCtxParams_registerSequenceProducer(&zc->requestedParams,
                                           extSeqProdState, extSeqProdFunc);
}

void ZSTD_CCtxParams_registerSequenceProducer(
    ZSTD_CCtx_params *params, void *extSeqProdState,
    ZSTD_sequenceProducer_F extSeqProdFunc) {
  assert(params != NULL);
  if (extSeqProdFunc != NULL) {
    params->extSeqProdFunc = extSeqProdFunc;
    params->extSeqProdState = extSeqProdState;
  } else {
    params->extSeqProdFunc = NULL;
    params->extSeqProdState = NULL;
  }
}
