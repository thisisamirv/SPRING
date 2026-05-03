/* ******************************************************************
 * debug
 * Part of FSE library
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * You can contact the author at :
 * - Source repository : https://github.com/Cyan4973/FiniteStateEntropy
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 ****************************************************************** */

#ifndef DEBUG_H_12987983217
#define DEBUG_H_12987983217

#define DEBUG_STATIC_ASSERT(c) (void)sizeof(char[(c) ? 1 : -1])

#ifndef DEBUGLEVEL
#define DEBUGLEVEL 0
#endif

#if (DEBUGLEVEL >= 1)
#define ZSTD_DEPS_NEED_ASSERT
#include "zstd_deps.h"
#else
#ifndef assert

#define assert(condition) ((void)0)
#endif
#endif

#if (DEBUGLEVEL >= 2)
#define ZSTD_DEPS_NEED_IO
#include "zstd_deps.h"
extern int g_debuglevel;

#define RAWLOG(l, ...)                                                         \
  do {                                                                         \
    if (l <= g_debuglevel) {                                                   \
      ZSTD_DEBUG_PRINT(__VA_ARGS__);                                           \
    }                                                                          \
  } while (0)

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define LINE_AS_STRING TOSTRING(__LINE__)

#define DEBUGLOG(l, ...)                                                       \
  do {                                                                         \
    if (l <= g_debuglevel) {                                                   \
      ZSTD_DEBUG_PRINT(__FILE__ ":" LINE_AS_STRING ": " __VA_ARGS__);          \
      ZSTD_DEBUG_PRINT(" \n");                                                 \
    }                                                                          \
  } while (0)
#else
#define RAWLOG(l, ...)                                                         \
  do {                                                                         \
  } while (0)
#define DEBUGLOG(l, ...)                                                       \
  do {                                                                         \
  } while (0)
#endif

#endif
