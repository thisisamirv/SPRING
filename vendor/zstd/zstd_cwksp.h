/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_CWKSP_H
#define ZSTD_CWKSP_H

#include "allocations.h"
#include "compiler.h"
#include "portability_macros.h"
#include "zstd_internal.h"

#ifndef ZSTD_CWKSP_ASAN_REDZONE_SIZE
#define ZSTD_CWKSP_ASAN_REDZONE_SIZE 128
#endif

#define ZSTD_CWKSP_ALIGNMENT_BYTES 64

typedef enum {
  ZSTD_cwksp_alloc_objects,
  ZSTD_cwksp_alloc_aligned_init_once,
  ZSTD_cwksp_alloc_aligned,
  ZSTD_cwksp_alloc_buffers
} ZSTD_cwksp_alloc_phase_e;

typedef enum {
  ZSTD_cwksp_dynamic_alloc,
  ZSTD_cwksp_static_alloc
} ZSTD_cwksp_static_alloc_e;

typedef struct {
  void *workspace;
  void *workspaceEnd;

  void *objectEnd;
  void *tableEnd;
  void *tableValidEnd;
  void *allocStart;
  void *initOnceStart;

  BYTE allocFailed;
  int workspaceOversizedDuration;
  ZSTD_cwksp_alloc_phase_e phase;
  ZSTD_cwksp_static_alloc_e isStatic;
} ZSTD_cwksp;

MEM_STATIC size_t ZSTD_cwksp_available_space(ZSTD_cwksp *ws);
MEM_STATIC void *ZSTD_cwksp_initialAllocStart(ZSTD_cwksp *ws);

MEM_STATIC void ZSTD_cwksp_assert_internal_consistency(ZSTD_cwksp *ws) {
  (void)ws;
  assert(ws->workspace <= ws->objectEnd);
  assert(ws->objectEnd <= ws->tableEnd);
  assert(ws->objectEnd <= ws->tableValidEnd);
  assert(ws->tableEnd <= ws->allocStart);
  assert(ws->tableValidEnd <= ws->allocStart);
  assert(ws->allocStart <= ws->workspaceEnd);
  assert(ws->initOnceStart <= ZSTD_cwksp_initialAllocStart(ws));
  assert(ws->workspace <= ws->initOnceStart);
#if ZSTD_MEMORY_SANITIZER
  {
    intptr_t const offset = __msan_test_shadow(
        ws->initOnceStart,
        (U8 *)ZSTD_cwksp_initialAllocStart(ws) - (U8 *)ws->initOnceStart);
    (void)offset;
#if defined(ZSTD_MSAN_PRINT)
    if (offset != -1) {
      __msan_print_shadow((U8 *)ws->initOnceStart + offset - 8, 32);
    }
#endif
    assert(offset == -1);
  };
#endif
}

MEM_STATIC size_t ZSTD_cwksp_align(size_t size, size_t align) {
  size_t const mask = align - 1;
  assert(ZSTD_isPower2(align));
  return (size + mask) & ~mask;
}

MEM_STATIC size_t ZSTD_cwksp_alloc_size(size_t size) {
  if (size == 0)
    return 0;
#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)
  return size + 2 * ZSTD_CWKSP_ASAN_REDZONE_SIZE;
#else
  return size;
#endif
}

MEM_STATIC size_t ZSTD_cwksp_aligned_alloc_size(size_t size, size_t alignment) {
  return ZSTD_cwksp_alloc_size(ZSTD_cwksp_align(size, alignment));
}

MEM_STATIC size_t ZSTD_cwksp_aligned64_alloc_size(size_t size) {
  return ZSTD_cwksp_aligned_alloc_size(size, ZSTD_CWKSP_ALIGNMENT_BYTES);
}

MEM_STATIC size_t ZSTD_cwksp_slack_space_required(void) {

  size_t const slackSpace = ZSTD_CWKSP_ALIGNMENT_BYTES * 2;
  return slackSpace;
}

MEM_STATIC size_t ZSTD_cwksp_bytes_to_align_ptr(void *ptr,
                                                const size_t alignBytes) {
  size_t const alignBytesMask = alignBytes - 1;
  size_t const bytes =
      (alignBytes - ((size_t)ptr & (alignBytesMask))) & alignBytesMask;
  assert(ZSTD_isPower2(alignBytes));
  assert(bytes < alignBytes);
  return bytes;
}

MEM_STATIC void *ZSTD_cwksp_initialAllocStart(ZSTD_cwksp *ws) {
  char *endPtr = (char *)ws->workspaceEnd;
  assert(ZSTD_isPower2(ZSTD_CWKSP_ALIGNMENT_BYTES));
  endPtr = endPtr - ((size_t)endPtr % ZSTD_CWKSP_ALIGNMENT_BYTES);
  return (void *)endPtr;
}

MEM_STATIC void *ZSTD_cwksp_reserve_internal_buffer_space(ZSTD_cwksp *ws,
                                                          size_t const bytes) {
  void *const alloc = (BYTE *)ws->allocStart - bytes;
  void *const bottom = ws->tableEnd;
  DEBUGLOG(5, "cwksp: reserving [0x%p]:%zd bytes; %zd bytes remaining", alloc,
           bytes, ZSTD_cwksp_available_space(ws) - bytes);
  ZSTD_cwksp_assert_internal_consistency(ws);
  assert(alloc >= bottom);
  if (alloc < bottom) {
    DEBUGLOG(4, "cwksp: alloc failed!");
    ws->allocFailed = 1;
    return NULL;
  }

  if (alloc < ws->tableValidEnd) {
    ws->tableValidEnd = alloc;
  }
  ws->allocStart = alloc;
  return alloc;
}

MEM_STATIC size_t ZSTD_cwksp_internal_advance_phase(
    ZSTD_cwksp *ws, ZSTD_cwksp_alloc_phase_e phase) {
  assert(phase >= ws->phase);
  if (phase > ws->phase) {

    if (ws->phase < ZSTD_cwksp_alloc_aligned_init_once &&
        phase >= ZSTD_cwksp_alloc_aligned_init_once) {
      ws->tableValidEnd = ws->objectEnd;
      ws->initOnceStart = ZSTD_cwksp_initialAllocStart(ws);

      {
        void *const alloc = ws->objectEnd;
        size_t const bytesToAlign =
            ZSTD_cwksp_bytes_to_align_ptr(alloc, ZSTD_CWKSP_ALIGNMENT_BYTES);
        void *const objectEnd = (BYTE *)alloc + bytesToAlign;
        DEBUGLOG(5, "reserving table alignment addtl space: %zu", bytesToAlign);
        RETURN_ERROR_IF(objectEnd > ws->workspaceEnd, memory_allocation,
                        "table phase - alignment initial allocation failed!");
        ws->objectEnd = objectEnd;
        ws->tableEnd = objectEnd;
        if (ws->tableValidEnd < ws->tableEnd) {
          ws->tableValidEnd = ws->tableEnd;
        }
      }
    }
    ws->phase = phase;
    ZSTD_cwksp_assert_internal_consistency(ws);
  }
  return 0;
}

MEM_STATIC int ZSTD_cwksp_owns_buffer(const ZSTD_cwksp *ws, const void *ptr) {
  return (ptr != NULL) && (ws->workspace <= ptr) && (ptr < ws->workspaceEnd);
}

MEM_STATIC void *ZSTD_cwksp_reserve_internal(ZSTD_cwksp *ws, size_t bytes,
                                             ZSTD_cwksp_alloc_phase_e phase) {
  void *alloc;
  if (ZSTD_isError(ZSTD_cwksp_internal_advance_phase(ws, phase)) ||
      bytes == 0) {
    return NULL;
  }

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  bytes += 2 * ZSTD_CWKSP_ASAN_REDZONE_SIZE;
#endif

  alloc = ZSTD_cwksp_reserve_internal_buffer_space(ws, bytes);

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  if (alloc) {
    alloc = (BYTE *)alloc + ZSTD_CWKSP_ASAN_REDZONE_SIZE;
    if (ws->isStatic == ZSTD_cwksp_dynamic_alloc) {

      __asan_unpoison_memory_region(alloc,
                                    bytes - 2 * ZSTD_CWKSP_ASAN_REDZONE_SIZE);
    }
  }
#endif

  return alloc;
}

MEM_STATIC BYTE *ZSTD_cwksp_reserve_buffer(ZSTD_cwksp *ws, size_t bytes) {
  return (BYTE *)ZSTD_cwksp_reserve_internal(ws, bytes,
                                             ZSTD_cwksp_alloc_buffers);
}

MEM_STATIC void *ZSTD_cwksp_reserve_aligned_init_once(ZSTD_cwksp *ws,
                                                      size_t bytes) {
  size_t const alignedBytes =
      ZSTD_cwksp_align(bytes, ZSTD_CWKSP_ALIGNMENT_BYTES);
  void *ptr = ZSTD_cwksp_reserve_internal(ws, alignedBytes,
                                          ZSTD_cwksp_alloc_aligned_init_once);
  assert(((size_t)ptr & (ZSTD_CWKSP_ALIGNMENT_BYTES - 1)) == 0);
  if (ptr && ptr < ws->initOnceStart) {

    ZSTD_memset(
        ptr, 0,
        MIN((size_t)((U8 *)ws->initOnceStart - (U8 *)ptr), alignedBytes));
    ws->initOnceStart = ptr;
  }
#if ZSTD_MEMORY_SANITIZER
  assert(__msan_test_shadow(ptr, bytes) == -1);
#endif
  return ptr;
}

MEM_STATIC void *ZSTD_cwksp_reserve_aligned64(ZSTD_cwksp *ws, size_t bytes) {
  void *const ptr = ZSTD_cwksp_reserve_internal(
      ws, ZSTD_cwksp_align(bytes, ZSTD_CWKSP_ALIGNMENT_BYTES),
      ZSTD_cwksp_alloc_aligned);
  assert(((size_t)ptr & (ZSTD_CWKSP_ALIGNMENT_BYTES - 1)) == 0);
  return ptr;
}

MEM_STATIC void *ZSTD_cwksp_reserve_table(ZSTD_cwksp *ws, size_t bytes) {
  const ZSTD_cwksp_alloc_phase_e phase = ZSTD_cwksp_alloc_aligned_init_once;
  void *alloc;
  void *end;
  void *top;

  if (ws->phase < phase) {
    if (ZSTD_isError(ZSTD_cwksp_internal_advance_phase(ws, phase))) {
      return NULL;
    }
  }
  alloc = ws->tableEnd;
  end = (BYTE *)alloc + bytes;
  top = ws->allocStart;

  DEBUGLOG(5, "cwksp: reserving %p table %zd bytes, %zd bytes remaining", alloc,
           bytes, ZSTD_cwksp_available_space(ws) - bytes);
  assert((bytes & (sizeof(U32) - 1)) == 0);
  ZSTD_cwksp_assert_internal_consistency(ws);
  assert(end <= top);
  if (end > top) {
    DEBUGLOG(4, "cwksp: table alloc failed!");
    ws->allocFailed = 1;
    return NULL;
  }
  ws->tableEnd = end;

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)
  if (ws->isStatic == ZSTD_cwksp_dynamic_alloc) {
    __asan_unpoison_memory_region(alloc, bytes);
  }
#endif

  assert((bytes & (ZSTD_CWKSP_ALIGNMENT_BYTES - 1)) == 0);
  assert(((size_t)alloc & (ZSTD_CWKSP_ALIGNMENT_BYTES - 1)) == 0);
  return alloc;
}

MEM_STATIC void *ZSTD_cwksp_reserve_object(ZSTD_cwksp *ws, size_t bytes) {
  size_t const roundedBytes = ZSTD_cwksp_align(bytes, sizeof(void *));
  void *alloc = ws->objectEnd;
  void *end = (BYTE *)alloc + roundedBytes;

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  end = (BYTE *)end + 2 * ZSTD_CWKSP_ASAN_REDZONE_SIZE;
#endif

  DEBUGLOG(4,
           "cwksp: reserving %p object %zd bytes (rounded to %zd), %zd bytes "
           "remaining",
           alloc, bytes, roundedBytes,
           ZSTD_cwksp_available_space(ws) - roundedBytes);
  assert((size_t)alloc % ZSTD_ALIGNOF(void *) == 0);
  assert(bytes % ZSTD_ALIGNOF(void *) == 0);
  ZSTD_cwksp_assert_internal_consistency(ws);

  if (ws->phase != ZSTD_cwksp_alloc_objects || end > ws->workspaceEnd) {
    DEBUGLOG(3, "cwksp: object alloc failed!");
    ws->allocFailed = 1;
    return NULL;
  }
  ws->objectEnd = end;
  ws->tableEnd = end;
  ws->tableValidEnd = end;

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  alloc = (BYTE *)alloc + ZSTD_CWKSP_ASAN_REDZONE_SIZE;
  if (ws->isStatic == ZSTD_cwksp_dynamic_alloc) {
    __asan_unpoison_memory_region(alloc, bytes);
  }
#endif

  return alloc;
}

MEM_STATIC void *ZSTD_cwksp_reserve_object_aligned(ZSTD_cwksp *ws,
                                                   size_t byteSize,
                                                   size_t alignment) {
  size_t const mask = alignment - 1;
  size_t const surplus =
      (alignment > sizeof(void *)) ? alignment - sizeof(void *) : 0;
  void *const start = ZSTD_cwksp_reserve_object(ws, byteSize + surplus);
  if (start == NULL)
    return NULL;
  if (surplus == 0)
    return start;
  assert(ZSTD_isPower2(alignment));
  return (void *)(((size_t)start + surplus) & ~mask);
}

MEM_STATIC void ZSTD_cwksp_mark_tables_dirty(ZSTD_cwksp *ws) {
  DEBUGLOG(4, "cwksp: ZSTD_cwksp_mark_tables_dirty");

#if ZSTD_MEMORY_SANITIZER && !defined(ZSTD_MSAN_DONT_POISON_WORKSPACE)

  {
    size_t size = (BYTE *)ws->tableValidEnd - (BYTE *)ws->objectEnd;
    assert(__msan_test_shadow(ws->objectEnd, size) == -1);
    if ((BYTE *)ws->tableValidEnd < (BYTE *)ws->initOnceStart) {
      __msan_poison(ws->objectEnd, size);
    } else {
      assert(ws->initOnceStart >= ws->objectEnd);
      __msan_poison(ws->objectEnd,
                    (BYTE *)ws->initOnceStart - (BYTE *)ws->objectEnd);
    }
  }
#endif

  assert(ws->tableValidEnd >= ws->objectEnd);
  assert(ws->tableValidEnd <= ws->allocStart);
  ws->tableValidEnd = ws->objectEnd;
  ZSTD_cwksp_assert_internal_consistency(ws);
}

MEM_STATIC void ZSTD_cwksp_mark_tables_clean(ZSTD_cwksp *ws) {
  DEBUGLOG(4, "cwksp: ZSTD_cwksp_mark_tables_clean");
  assert(ws->tableValidEnd >= ws->objectEnd);
  assert(ws->tableValidEnd <= ws->allocStart);
  if (ws->tableValidEnd < ws->tableEnd) {
    ws->tableValidEnd = ws->tableEnd;
  }
  ZSTD_cwksp_assert_internal_consistency(ws);
}

MEM_STATIC void ZSTD_cwksp_clean_tables(ZSTD_cwksp *ws) {
  DEBUGLOG(4, "cwksp: ZSTD_cwksp_clean_tables");
  assert(ws->tableValidEnd >= ws->objectEnd);
  assert(ws->tableValidEnd <= ws->allocStart);
  if (ws->tableValidEnd < ws->tableEnd) {
    ZSTD_memset(ws->tableValidEnd, 0,
                (size_t)((BYTE *)ws->tableEnd - (BYTE *)ws->tableValidEnd));
  }
  ZSTD_cwksp_mark_tables_clean(ws);
}

MEM_STATIC void ZSTD_cwksp_clear_tables(ZSTD_cwksp *ws) {
  DEBUGLOG(4, "cwksp: clearing tables!");

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  if (ws->isStatic == ZSTD_cwksp_dynamic_alloc) {
    size_t size = (BYTE *)ws->tableValidEnd - (BYTE *)ws->objectEnd;
    __asan_poison_memory_region(ws->objectEnd, size);
  }
#endif

  ws->tableEnd = ws->objectEnd;
  ZSTD_cwksp_assert_internal_consistency(ws);
}

MEM_STATIC void ZSTD_cwksp_clear(ZSTD_cwksp *ws) {
  DEBUGLOG(4, "cwksp: clearing!");

#if ZSTD_MEMORY_SANITIZER && !defined(ZSTD_MSAN_DONT_POISON_WORKSPACE)

  {
    if ((BYTE *)ws->tableValidEnd < (BYTE *)ws->initOnceStart) {
      size_t size = (BYTE *)ws->initOnceStart - (BYTE *)ws->tableValidEnd;
      __msan_poison(ws->tableValidEnd, size);
    }
  }
#endif

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

  if (ws->isStatic == ZSTD_cwksp_dynamic_alloc) {
    size_t size = (BYTE *)ws->workspaceEnd - (BYTE *)ws->objectEnd;
    __asan_poison_memory_region(ws->objectEnd, size);
  }
#endif

  ws->tableEnd = ws->objectEnd;
  ws->allocStart = ZSTD_cwksp_initialAllocStart(ws);
  ws->allocFailed = 0;
  if (ws->phase > ZSTD_cwksp_alloc_aligned_init_once) {
    ws->phase = ZSTD_cwksp_alloc_aligned_init_once;
  }
  ZSTD_cwksp_assert_internal_consistency(ws);
}

MEM_STATIC size_t ZSTD_cwksp_sizeof(const ZSTD_cwksp *ws) {
  return (size_t)((BYTE *)ws->workspaceEnd - (BYTE *)ws->workspace);
}

MEM_STATIC size_t ZSTD_cwksp_used(const ZSTD_cwksp *ws) {
  return (size_t)((BYTE *)ws->tableEnd - (BYTE *)ws->workspace) +
         (size_t)((BYTE *)ws->workspaceEnd - (BYTE *)ws->allocStart);
}

MEM_STATIC void ZSTD_cwksp_init(ZSTD_cwksp *ws, void *start, size_t size,
                                ZSTD_cwksp_static_alloc_e isStatic) {
  DEBUGLOG(4, "cwksp: init'ing workspace with %zd bytes", size);
  assert(((size_t)start & (sizeof(void *) - 1)) == 0);
  ws->workspace = start;
  ws->workspaceEnd = (BYTE *)start + size;
  ws->objectEnd = ws->workspace;
  ws->tableValidEnd = ws->objectEnd;
  ws->initOnceStart = ZSTD_cwksp_initialAllocStart(ws);
  ws->phase = ZSTD_cwksp_alloc_objects;
  ws->isStatic = isStatic;
  ZSTD_cwksp_clear(ws);
  ws->workspaceOversizedDuration = 0;
  ZSTD_cwksp_assert_internal_consistency(ws);
}

MEM_STATIC size_t ZSTD_cwksp_create(ZSTD_cwksp *ws, size_t size,
                                    ZSTD_customMem customMem) {
  void *workspace = ZSTD_customMalloc(size, customMem);
  DEBUGLOG(4, "cwksp: creating new workspace with %zd bytes", size);
  RETURN_ERROR_IF(workspace == NULL, memory_allocation, "NULL pointer!");
  ZSTD_cwksp_init(ws, workspace, size, ZSTD_cwksp_dynamic_alloc);
  return 0;
}

MEM_STATIC void ZSTD_cwksp_free(ZSTD_cwksp *ws, ZSTD_customMem customMem) {
  void *ptr = ws->workspace;
  DEBUGLOG(4, "cwksp: freeing workspace");
#if ZSTD_MEMORY_SANITIZER && !defined(ZSTD_MSAN_DONT_POISON_WORKSPACE)
  if (ptr != NULL && customMem.customFree != NULL) {
    __msan_unpoison(ptr, ZSTD_cwksp_sizeof(ws));
  }
#endif
  ZSTD_memset(ws, 0, sizeof(ZSTD_cwksp));
  ZSTD_customFree(ptr, customMem);
}

MEM_STATIC void ZSTD_cwksp_move(ZSTD_cwksp *dst, ZSTD_cwksp *src) {
  *dst = *src;
  ZSTD_memset(src, 0, sizeof(ZSTD_cwksp));
}

MEM_STATIC int ZSTD_cwksp_reserve_failed(const ZSTD_cwksp *ws) {
  return ws->allocFailed;
}

MEM_STATIC int
ZSTD_cwksp_estimated_space_within_bounds(const ZSTD_cwksp *const ws,
                                         size_t const estimatedSpace) {

  return (estimatedSpace - ZSTD_cwksp_slack_space_required()) <=
             ZSTD_cwksp_used(ws) &&
         ZSTD_cwksp_used(ws) <= estimatedSpace;
}

MEM_STATIC size_t ZSTD_cwksp_available_space(ZSTD_cwksp *ws) {
  return (size_t)((BYTE *)ws->allocStart - (BYTE *)ws->tableEnd);
}

MEM_STATIC int ZSTD_cwksp_check_available(ZSTD_cwksp *ws,
                                          size_t additionalNeededSpace) {
  return ZSTD_cwksp_available_space(ws) >= additionalNeededSpace;
}

MEM_STATIC int ZSTD_cwksp_check_too_large(ZSTD_cwksp *ws,
                                          size_t additionalNeededSpace) {
  return ZSTD_cwksp_check_available(ws, additionalNeededSpace *
                                            ZSTD_WORKSPACETOOLARGE_FACTOR);
}

MEM_STATIC int ZSTD_cwksp_check_wasteful(ZSTD_cwksp *ws,
                                         size_t additionalNeededSpace) {
  return ZSTD_cwksp_check_too_large(ws, additionalNeededSpace) &&
         ws->workspaceOversizedDuration > ZSTD_WORKSPACETOOLARGE_MAXDURATION;
}

MEM_STATIC void
ZSTD_cwksp_bump_oversized_duration(ZSTD_cwksp *ws,
                                   size_t additionalNeededSpace) {
  if (ZSTD_cwksp_check_too_large(ws, additionalNeededSpace)) {
    ws->workspaceOversizedDuration++;
  } else {
    ws->workspaceOversizedDuration = 0;
  }
}

#endif
