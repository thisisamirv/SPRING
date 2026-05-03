/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_COMPRESS_ADVANCED_H
#define ZSTD_COMPRESS_ADVANCED_H

#include "zstd.h"

size_t ZSTD_compressSuperBlock(ZSTD_CCtx *zc, void *dst, size_t dstCapacity,
                               void const *src, size_t srcSize,
                               unsigned lastBlock);

#endif
