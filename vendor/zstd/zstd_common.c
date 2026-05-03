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
#include "error_private.h"
#include "zstd_internal.h"

unsigned ZSTD_versionNumber(void) { return ZSTD_VERSION_NUMBER; }

const char *ZSTD_versionString(void) { return ZSTD_VERSION_STRING; }

#undef ZSTD_isError

unsigned ZSTD_isError(size_t code) { return ERR_isError(code); }

const char *ZSTD_getErrorName(size_t code) { return ERR_getErrorName(code); }

ZSTD_ErrorCode ZSTD_getErrorCode(size_t code) { return ERR_getErrorCode(code); }

const char *ZSTD_getErrorString(ZSTD_ErrorCode code) {
  return ERR_getErrorString(code);
}

int ZSTD_isDeterministicBuild(void) {
#if ZSTD_IS_DETERMINISTIC_BUILD
  return 1;
#else
  return 0;
#endif
}
