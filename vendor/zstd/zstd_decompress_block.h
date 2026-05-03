/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_DEC_BLOCK_H
#define ZSTD_DEC_BLOCK_H

#include "zstd.h"
#include "zstd_decompress_internal.h"
#include "zstd_deps.h"

typedef enum { not_streaming = 0, is_streaming = 1 } streaming_operation;

size_t ZSTD_decompressBlock_internal(ZSTD_DCtx *dctx, void *dst,
                                     size_t dstCapacity, const void *src,
                                     size_t srcSize,
                                     streaming_operation streaming);

void ZSTD_buildFSETable(ZSTD_seqSymbol *dt, const short *normalizedCounter,
                        unsigned maxSymbolValue, const U32 *baseValue,
                        const U8 *nbAdditionalBits, unsigned tableLog,
                        void *wksp, size_t wkspSize, int bmi2);

size_t ZSTD_decompressBlock_deprecated(ZSTD_DCtx *dctx, void *dst,
                                       size_t dstCapacity, const void *src,
                                       size_t srcSize);

#endif
