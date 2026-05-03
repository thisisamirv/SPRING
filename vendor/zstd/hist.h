/* ******************************************************************
 * hist : Histogram functions
 * part of Finite State Entropy project
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 *  You can contact the author at :
 *  - FSE source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *  - Public forum : https://groups.google.com/forum/#!forum/lz4c
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 ****************************************************************** */

#include "zstd_deps.h"

size_t HIST_count(unsigned *count, unsigned *maxSymbolValuePtr, const void *src,
                  size_t srcSize);

unsigned HIST_isError(size_t code);

#if defined(__ARM_FEATURE_SVE2)
#define HIST_WKSP_SIZE_U32 0
#else
#define HIST_WKSP_SIZE_U32 1024
#endif
#define HIST_WKSP_SIZE (HIST_WKSP_SIZE_U32 * sizeof(unsigned))

size_t HIST_count_wksp(unsigned *count, unsigned *maxSymbolValuePtr,
                       const void *src, size_t srcSize, void *workSpace,
                       size_t workSpaceSize);

size_t HIST_countFast(unsigned *count, unsigned *maxSymbolValuePtr,
                      const void *src, size_t srcSize);

size_t HIST_countFast_wksp(unsigned *count, unsigned *maxSymbolValuePtr,
                           const void *src, size_t srcSize, void *workSpace,
                           size_t workSpaceSize);

unsigned HIST_count_simple(unsigned *count, unsigned *maxSymbolValuePtr,
                           const void *src, size_t srcSize);

void HIST_add(unsigned *count, const void *src, size_t srcSize);
