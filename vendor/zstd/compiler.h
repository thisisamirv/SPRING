/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_COMPILER_H
#define ZSTD_COMPILER_H

#include <stddef.h>

#include "portability_macros.h"

#if !defined(ZSTD_NO_INLINE)
#if (defined(__GNUC__) && !defined(__STRICT_ANSI__)) ||                        \
    defined(__cplusplus) ||                                                    \
    defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define INLINE_KEYWORD inline
#else
#define INLINE_KEYWORD
#endif

#if defined(__GNUC__) || defined(__IAR_SYSTEMS_ICC__)
#define FORCE_INLINE_ATTR __attribute__((always_inline))
#elif defined(_MSC_VER)
#define FORCE_INLINE_ATTR __forceinline
#else
#define FORCE_INLINE_ATTR
#endif

#else

#define INLINE_KEYWORD
#define FORCE_INLINE_ATTR

#endif

#if defined(_MSC_VER)
#define WIN_CDECL __cdecl
#else
#define WIN_CDECL
#endif

#if defined(__GNUC__) || defined(__IAR_SYSTEMS_ICC__)
#define UNUSED_ATTR __attribute__((unused))
#else
#define UNUSED_ATTR
#endif

#define FORCE_INLINE_TEMPLATE                                                  \
  static INLINE_KEYWORD FORCE_INLINE_ATTR UNUSED_ATTR

#if !defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 4 &&               \
    __GNUC_MINOR__ >= 8 && __GNUC__ < 5
#define HINT_INLINE static INLINE_KEYWORD
#else
#define HINT_INLINE FORCE_INLINE_TEMPLATE
#endif

#ifndef MEM_STATIC
#if defined(__GNUC__)
#define MEM_STATIC static __inline UNUSED_ATTR
#elif defined(__IAR_SYSTEMS_ICC__)
#define MEM_STATIC static inline UNUSED_ATTR
#elif defined(__cplusplus) ||                                                  \
    (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L))
#define MEM_STATIC static inline
#elif defined(_MSC_VER)
#define MEM_STATIC static __inline
#else
#define MEM_STATIC static

#endif
#endif

#ifdef _MSC_VER
#define FORCE_NOINLINE static __declspec(noinline)
#else
#if defined(__GNUC__) || defined(__IAR_SYSTEMS_ICC__)
#define FORCE_NOINLINE static __attribute__((__noinline__))
#else
#define FORCE_NOINLINE static
#endif
#endif

#if defined(__GNUC__) || defined(__IAR_SYSTEMS_ICC__)
#define TARGET_ATTRIBUTE(target) __attribute__((__target__(target)))
#else
#define TARGET_ATTRIBUTE(target)
#endif

#define BMI2_TARGET_ATTRIBUTE TARGET_ATTRIBUTE("lzcnt,bmi,bmi2")

#if defined(NO_PREFETCH)
#define PREFETCH_L1(ptr)                                                       \
  do {                                                                         \
    (void)(ptr);                                                               \
  } while (0)
#define PREFETCH_L2(ptr)                                                       \
  do {                                                                         \
    (void)(ptr);                                                               \
  } while (0)
#else
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_I86)) &&               \
    !defined(_M_ARM64EC)
#include <mmintrin.h>
#define PREFETCH_L1(ptr) _mm_prefetch((const char *)(ptr), _MM_HINT_T0)
#define PREFETCH_L2(ptr) _mm_prefetch((const char *)(ptr), _MM_HINT_T1)
#elif defined(__GNUC__) &&                                                     \
    ((__GNUC__ >= 4) || ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 1)))
#define PREFETCH_L1(ptr) __builtin_prefetch((ptr), 0, 3)
#define PREFETCH_L2(ptr) __builtin_prefetch((ptr), 0, 2)
#elif defined(__aarch64__)
#define PREFETCH_L1(ptr)                                                       \
  do {                                                                         \
    __asm__ __volatile__("prfm pldl1keep, %0" ::"Q"(*(ptr)));                  \
  } while (0)
#define PREFETCH_L2(ptr)                                                       \
  do {                                                                         \
    __asm__ __volatile__("prfm pldl2keep, %0" ::"Q"(*(ptr)));                  \
  } while (0)
#else
#define PREFETCH_L1(ptr)                                                       \
  do {                                                                         \
    (void)(ptr);                                                               \
  } while (0)
#define PREFETCH_L2(ptr)                                                       \
  do {                                                                         \
    (void)(ptr);                                                               \
  } while (0)
#endif
#endif

#define CACHELINE_SIZE 64

#define PREFETCH_AREA(p, s)                                                    \
  do {                                                                         \
    const char *const _ptr = (const char *)(p);                                \
    size_t const _size = (size_t)(s);                                          \
    size_t _pos;                                                               \
    for (_pos = 0; _pos < _size; _pos += CACHELINE_SIZE) {                     \
      PREFETCH_L2(_ptr + _pos);                                                \
    }                                                                          \
  } while (0)

#if !defined(__INTEL_COMPILER) && !defined(__clang__) && defined(__GNUC__) &&  \
    !defined(__LCC__)
#if (__GNUC__ == 4 && __GNUC_MINOR__ > 3) || (__GNUC__ >= 5)
#define DONT_VECTORIZE __attribute__((optimize("no-tree-vectorize")))
#else
#define DONT_VECTORIZE _Pragma("GCC optimize(\"no-tree-vectorize\")")
#endif
#else
#define DONT_VECTORIZE
#endif

#if defined(__GNUC__)
#define LIKELY(x) (__builtin_expect((x), 1))
#define UNLIKELY(x) (__builtin_expect((x), 0))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#if __has_builtin(__builtin_unreachable) ||                                    \
    (defined(__GNUC__) &&                                                      \
     (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)))
#define ZSTD_UNREACHABLE                                                       \
  do {                                                                         \
    assert(0), __builtin_unreachable();                                        \
  } while (0)
#else
#define ZSTD_UNREACHABLE                                                       \
  do {                                                                         \
    assert(0);                                                                 \
  } while (0)
#endif

#ifdef _MSC_VER
#include <intrin.h>
#pragma warning(disable : 4100)
#pragma warning(disable : 4127)
#pragma warning(disable : 4204)
#pragma warning(disable : 4214)
#pragma warning(disable : 4324)
#endif

#if !defined(ZSTD_NO_INTRINSICS)
#if defined(__AVX2__)
#define ZSTD_ARCH_X86_AVX2
#endif
#if defined(__SSE2__) || defined(_M_X64) ||                                    \
    (defined(_M_IX86) && defined(_M_IX86_FP) && (_M_IX86_FP >= 2))
#define ZSTD_ARCH_X86_SSE2
#endif
#if defined(__ARM_NEON) || defined(_M_ARM64)
#define ZSTD_ARCH_ARM_NEON
#endif
#if defined(__ARM_FEATURE_SVE)
#define ZSTD_ARCH_ARM_SVE
#endif
#if defined(__ARM_FEATURE_SVE2)
#define ZSTD_ARCH_ARM_SVE2
#endif
#if defined(__riscv) && defined(__riscv_vector)
#if ((defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14) ||           \
     (defined(__clang__) && __clang_major__ >= 19))
#define ZSTD_ARCH_RISCV_RVV
#endif
#endif
#if defined(ZSTD_ARCH_X86_AVX2)
#include <immintrin.h>
#endif
#if defined(ZSTD_ARCH_X86_SSE2)
#include <emmintrin.h>
#elif defined(ZSTD_ARCH_ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(ZSTD_ARCH_ARM_SVE) || defined(ZSTD_ARCH_ARM_SVE2)
#include <arm_sve.h>
#endif
#if defined(ZSTD_ARCH_RISCV_RVV)
#include <riscv_vector.h>
#endif
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ > 201710L) &&               \
    defined(__has_c_attribute)
#define ZSTD_HAS_C_ATTRIBUTE(x) __has_c_attribute(x)
#else
#define ZSTD_HAS_C_ATTRIBUTE(x) 0
#endif

#if defined(__cplusplus) && defined(__has_cpp_attribute)
#define ZSTD_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#define ZSTD_HAS_CPP_ATTRIBUTE(x) 0
#endif

#ifndef ZSTD_FALLTHROUGH
#if ZSTD_HAS_C_ATTRIBUTE(fallthrough)
#define ZSTD_FALLTHROUGH [[fallthrough]]
#elif ZSTD_HAS_CPP_ATTRIBUTE(fallthrough)
#define ZSTD_FALLTHROUGH [[fallthrough]]
#elif __has_attribute(__fallthrough__)

#define ZSTD_FALLTHROUGH                                                       \
  ;                                                                            \
  __attribute__((__fallthrough__))
#else
#define ZSTD_FALLTHROUGH
#endif
#endif

MEM_STATIC int ZSTD_isPower2(size_t u) { return (u & (u - 1)) == 0; }

#ifndef ZSTD_ALIGNOF
#if defined(__GNUC__) || defined(_MSC_VER)

#define ZSTD_ALIGNOF(T) __alignof(T)

#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)

#include <stdalign.h>
#define ZSTD_ALIGNOF(T) alignof(T)

#else

#define ZSTD_ALIGNOF(T)                                                        \
  (sizeof(void *) < sizeof(T) ? sizeof(void *) : sizeof(T))

#endif
#endif

#ifndef ZSTD_ALIGNED

#if defined(__GNUC__) || defined(__clang__)
#define ZSTD_ALIGNED(a) __attribute__((aligned(a)))
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define ZSTD_ALIGNED(a) _Alignas(a)
#elif defined(_MSC_VER)
#define ZSTD_ALIGNED(n) __declspec(align(n))
#else

#define ZSTD_ALIGNED(...)
#endif
#endif

#ifndef ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
#if __has_attribute(no_sanitize)
#if !defined(__clang__) && defined(__GNUC__) && __GNUC__ < 8

#define ZSTD_ALLOW_POINTER_OVERFLOW_ATTR                                       \
  __attribute__((no_sanitize("signed-integer-overflow")))
#else

#define ZSTD_ALLOW_POINTER_OVERFLOW_ATTR                                       \
  __attribute__((no_sanitize("pointer-overflow")))
#endif
#else
#define ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
#endif
#endif

MEM_STATIC
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
ptrdiff_t ZSTD_wrappedPtrDiff(unsigned char const *lhs,
                              unsigned char const *rhs) {
  return lhs - rhs;
}

MEM_STATIC
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
const void *ZSTD_wrappedPtrAdd(const void *ptr, ptrdiff_t add) {
  return (const char *)ptr + add;
}

MEM_STATIC
ZSTD_ALLOW_POINTER_OVERFLOW_ATTR
const void *ZSTD_wrappedPtrSub(const void *ptr, ptrdiff_t sub) {
  return (const char *)ptr - sub;
}

MEM_STATIC
void *ZSTD_maybeNullPtrAdd(void *ptr, ptrdiff_t add) {
  return add > 0 ? (char *)ptr + add : ptr;
}

#ifdef __MINGW32__
#ifndef ZSTD_ASAN_DONT_POISON_WORKSPACE
#define ZSTD_ASAN_DONT_POISON_WORKSPACE 1
#endif
#ifndef ZSTD_MSAN_DONT_POISON_WORKSPACE
#define ZSTD_MSAN_DONT_POISON_WORKSPACE 1
#endif
#endif

#if ZSTD_MEMORY_SANITIZER && !defined(ZSTD_MSAN_DONT_POISON_WORKSPACE)

#include <stddef.h>
#define ZSTD_DEPS_NEED_STDINT
#include "zstd_deps.h"

void __msan_unpoison(const volatile void *a, size_t size);

void __msan_poison(const volatile void *a, size_t size);

intptr_t __msan_test_shadow(const volatile void *x, size_t size);

void __msan_print_shadow(const volatile void *x, size_t size);
#endif

#if ZSTD_ADDRESS_SANITIZER && !defined(ZSTD_ASAN_DONT_POISON_WORKSPACE)

#include <stddef.h>

void __asan_poison_memory_region(void const volatile *addr, size_t size);

void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#endif

#endif
