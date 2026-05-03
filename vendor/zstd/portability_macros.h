/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_PORTABILITY_MACROS_H
#define ZSTD_PORTABILITY_MACROS_H

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#ifndef ZSTD_MEMORY_SANITIZER
#if __has_feature(memory_sanitizer)
#define ZSTD_MEMORY_SANITIZER 1
#else
#define ZSTD_MEMORY_SANITIZER 0
#endif
#endif

#ifndef ZSTD_ADDRESS_SANITIZER
#if __has_feature(address_sanitizer)
#define ZSTD_ADDRESS_SANITIZER 1
#elif defined(__SANITIZE_ADDRESS__)
#define ZSTD_ADDRESS_SANITIZER 1
#else
#define ZSTD_ADDRESS_SANITIZER 0
#endif
#endif

#ifndef ZSTD_DATAFLOW_SANITIZER
#if __has_feature(dataflow_sanitizer)
#define ZSTD_DATAFLOW_SANITIZER 1
#else
#define ZSTD_DATAFLOW_SANITIZER 0
#endif
#endif

#ifdef __ELF__
#define ZSTD_HIDE_ASM_FUNCTION(func) .hidden func
#elif defined(__APPLE__)
#define ZSTD_HIDE_ASM_FUNCTION(func) .private_extern func
#else
#define ZSTD_HIDE_ASM_FUNCTION(func)
#endif

#ifndef STATIC_BMI2
#if defined(__BMI2__)
#define STATIC_BMI2 1
#elif defined(_MSC_VER) && defined(__AVX2__)
#define STATIC_BMI2 1

#endif
#endif

#ifndef STATIC_BMI2
#define STATIC_BMI2 0
#endif

#ifndef DYNAMIC_BMI2
#if ((defined(__clang__) && __has_attribute(__target__)) ||                    \
     (defined(__GNUC__) &&                                                     \
      (__GNUC__ >= 5 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8)))) &&           \
    (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) ||           \
     defined(_M_X64)) &&                                                       \
    !defined(__BMI2__)
#define DYNAMIC_BMI2 1
#else
#define DYNAMIC_BMI2 0
#endif
#endif

#if defined(__GNUC__)
#if defined(__linux__) || defined(__linux) || defined(__APPLE__) ||            \
    defined(_WIN32)
#if ZSTD_MEMORY_SANITIZER
#define ZSTD_ASM_SUPPORTED 0
#elif ZSTD_DATAFLOW_SANITIZER
#define ZSTD_ASM_SUPPORTED 0
#else
#define ZSTD_ASM_SUPPORTED 1
#endif
#else
#define ZSTD_ASM_SUPPORTED 0
#endif
#else
#define ZSTD_ASM_SUPPORTED 0
#endif

#if !defined(ZSTD_DISABLE_ASM) && ZSTD_ASM_SUPPORTED && defined(__x86_64__) && \
    (DYNAMIC_BMI2 || defined(__BMI2__))
#define ZSTD_ENABLE_ASM_X86_64_BMI2 1
#else
#define ZSTD_ENABLE_ASM_X86_64_BMI2 0
#endif

#if defined(__ELF__) && (defined(__x86_64__) || defined(__i386__)) &&          \
    defined(__has_include)
#if __has_include(<cet.h>)
#include <cet.h>
#define ZSTD_CET_ENDBRANCH _CET_ENDBR
#endif
#endif

#ifndef ZSTD_CET_ENDBRANCH
#define ZSTD_CET_ENDBRANCH
#endif

#if defined(ZSTD_CLEVEL_DEFAULT) ||                                            \
    defined(ZSTD_EXCLUDE_DFAST_BLOCK_COMPRESSOR) ||                            \
    defined(ZSTD_EXCLUDE_GREEDY_BLOCK_COMPRESSOR) ||                           \
    defined(ZSTD_EXCLUDE_LAZY_BLOCK_COMPRESSOR) ||                             \
    defined(ZSTD_EXCLUDE_LAZY2_BLOCK_COMPRESSOR) ||                            \
    defined(ZSTD_EXCLUDE_BTLAZY2_BLOCK_COMPRESSOR) ||                          \
    defined(ZSTD_EXCLUDE_BTOPT_BLOCK_COMPRESSOR) ||                            \
    defined(ZSTD_EXCLUDE_BTULTRA_BLOCK_COMPRESSOR)
#define ZSTD_IS_DETERMINISTIC_BUILD 0
#else
#define ZSTD_IS_DETERMINISTIC_BUILD 1
#endif

#endif
