/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_DDICT_H
#define ZSTD_DDICT_H

#include "zstd.h"
#include "zstd_deps.h"

const void *ZSTD_DDict_dictContent(const ZSTD_DDict *ddict);
size_t ZSTD_DDict_dictSize(const ZSTD_DDict *ddict);

void ZSTD_copyDDictParameters(ZSTD_DCtx *dctx, const ZSTD_DDict *ddict);

#endif
