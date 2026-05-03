/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_DEPS_COMMON
#define ZSTD_DEPS_COMMON

#if defined(__linux) || defined(__linux__) || defined(linux) ||                \
    defined(__gnu_linux__) || defined(__CYGWIN__) || defined(__MSYS__)
#if !defined(_GNU_SOURCE) && !defined(__ANDROID__)
#define _GNU_SOURCE
#endif
#endif

#include <limits.h>
#include <stddef.h>
#if defined(__GNUC__) && __GNUC__ >= 4
#define ZSTD_memcpy(d, s, l) __builtin_memcpy((d), (s), (l))
#define ZSTD_memmove(d, s, l) __builtin_memmove((d), (s), (l))
#define ZSTD_memset(p, v, l) __builtin_memset((p), (v), (l))
#else
#define ZSTD_memcpy(d, s, l) memcpy((d), (s), (l))
#define ZSTD_memmove(d, s, l) memmove((d), (s), (l))
#define ZSTD_memset(p, v, l) memset((p), (v), (l))
#endif

#endif

#ifdef ZSTD_DEPS_NEED_MALLOC
#ifndef ZSTD_DEPS_MALLOC
#define ZSTD_DEPS_MALLOC

#include <stdlib.h>

#define ZSTD_malloc(s) malloc(s)
#define ZSTD_calloc(n, s) calloc((n), (s))
#define ZSTD_free(p) free((p))

#endif
#endif

#ifdef ZSTD_DEPS_NEED_MATH64
#ifndef ZSTD_DEPS_MATH64
#define ZSTD_DEPS_MATH64

#define ZSTD_div64(dividend, divisor) ((dividend) / (divisor))

#endif
#endif

#ifdef ZSTD_DEPS_NEED_ASSERT
#ifndef ZSTD_DEPS_ASSERT
#define ZSTD_DEPS_ASSERT

#include <assert.h>

#endif
#endif

#ifdef ZSTD_DEPS_NEED_IO
#ifndef ZSTD_DEPS_IO
#define ZSTD_DEPS_IO

#include <stdio.h>
#define ZSTD_DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)

#endif
#endif

#ifdef ZSTD_DEPS_NEED_STDINT
#ifndef ZSTD_DEPS_STDINT
#define ZSTD_DEPS_STDINT

#include <stdint.h>

#endif
#endif
