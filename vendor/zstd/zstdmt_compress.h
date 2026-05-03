/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDMT_COMPRESS_H
#define ZSTDMT_COMPRESS_H

#include "zstd_deps.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#ifndef ZSTDMT_NBWORKERS_MAX

#define ZSTDMT_NBWORKERS_MAX ((sizeof(void *) == 4) ? 64 : 256)
#endif
#ifndef ZSTDMT_JOBSIZE_MIN

#define ZSTDMT_JOBSIZE_MIN (512 KB)
#endif
#define ZSTDMT_JOBLOG_MAX (MEM_32bits() ? 29 : 30)
#define ZSTDMT_JOBSIZE_MAX (MEM_32bits() ? (512 MB) : (1024 MB))

typedef struct ZSTDMT_CCtx_s ZSTDMT_CCtx;

ZSTDMT_CCtx *ZSTDMT_createCCtx_advanced(unsigned nbWorkers, ZSTD_customMem cMem,
                                        ZSTD_threadPool *pool);
size_t ZSTDMT_freeCCtx(ZSTDMT_CCtx *mtctx);

size_t ZSTDMT_sizeof_CCtx(ZSTDMT_CCtx *mtctx);

size_t ZSTDMT_nextInputSizeHint(const ZSTDMT_CCtx *mtctx);

size_t ZSTDMT_initCStream_internal(ZSTDMT_CCtx *mtctx, const void *dict,
                                   size_t dictSize,
                                   ZSTD_dictContentType_e dictContentType,
                                   const ZSTD_CDict *cdict,
                                   ZSTD_CCtx_params params,
                                   unsigned long long pledgedSrcSize);

size_t ZSTDMT_compressStream_generic(ZSTDMT_CCtx *mtctx, ZSTD_outBuffer *output,
                                     ZSTD_inBuffer *input,
                                     ZSTD_EndDirective endOp);

size_t ZSTDMT_toFlushNow(ZSTDMT_CCtx *mtctx);

void ZSTDMT_updateCParams_whileCompressing(ZSTDMT_CCtx *mtctx,
                                           const ZSTD_CCtx_params *cctxParams);

ZSTD_frameProgression ZSTDMT_getFrameProgression(ZSTDMT_CCtx *mtctx);

#endif
