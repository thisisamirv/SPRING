/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#define ZSTD_DEPS_NEED_MALLOC
#include "zstd_deps.h"
#include <string.h>

#include "compiler.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#ifndef ZSTD_ALLOCATIONS_H
#define ZSTD_ALLOCATIONS_H

MEM_STATIC void *ZSTD_customMalloc(size_t size, ZSTD_customMem customMem) {
  if (customMem.customAlloc)
    return customMem.customAlloc(customMem.opaque, size);
  return ZSTD_malloc(size);
}

MEM_STATIC void *ZSTD_customCalloc(size_t size, ZSTD_customMem customMem) {
  if (customMem.customAlloc) {

    void *const ptr = customMem.customAlloc(customMem.opaque, size);

    if (ptr == NULL) {
      return NULL;
    }

    ZSTD_memset(ptr, 0, size);
    return ptr;
  }
  return ZSTD_calloc(1, size);
}

MEM_STATIC void ZSTD_customFree(void *ptr, ZSTD_customMem customMem) {
  if (ptr != NULL) {
    if (customMem.customFree)
      customMem.customFree(customMem.opaque, ptr);
    else
      ZSTD_free(ptr);
  }
}

#endif
