/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_PRESPLIT_H
#define ZSTD_PRESPLIT_H

#include <stddef.h>

#define ZSTD_SLIPBLOCK_WORKSPACESIZE 8208

size_t ZSTD_splitBlock(const void *blockStart, size_t blockSize, int level,
                       void *workspace, size_t wkspSize);

#endif
