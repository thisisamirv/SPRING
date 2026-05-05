#!/usr/bin/env bash

set -euo pipefail

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "$0")" && pwd)/../common/common.sh"

require_command cppcheck

ZSTD_INCLUDE_DIR="$ROOT_DIR/vendor/zstd/lib"
LIBBSC_INCLUDE_DIR="$ROOT_DIR/vendor/libbsc"
LIBDEFLATE_INCLUDE_DIR="$ROOT_DIR/vendor/libdeflate"
LIBARCHIVE_INCLUDE_DIR="$ROOT_DIR/vendor/libarchive/lib"
ZLIB_INCLUDE_DIR="$ROOT_DIR/vendor/cloudflare_zlib"
BZIP2_INCLUDE_DIR="$ROOT_DIR/vendor/indexed_bzip2/src"
PTHASH_INCLUDE_DIR="$ROOT_DIR/vendor/pthash/include"
PTHASH_EXTERNAL_DIR="$ROOT_DIR/vendor/pthash/external"
QVZ_INCLUDE_DIR="$ROOT_DIR/vendor/qvz/include"

if [[ $# -gt 0 ]]; then
	targets=()
	for arg in "$@"; do
		targets+=("$(normalize_repo_path "$arg")")
	done
else
	targets=("$ROOT_DIR/src" "$ROOT_DIR/vendor" "$ROOT_DIR/tests")
fi

if [[ ${#targets[@]} -eq 0 ]]; then
	echo "No targets selected for cppcheck." >&2
	exit 1
fi

cppcheck \
	--error-exitcode=1 \
	--enable=warning,performance,portability \
	--suppress=missingInclude \
	--suppress=missingIncludeSystem \
	--suppress=normalCheckLevelMaxBranches \
	--suppress=toomanyconfigs \
	--suppress="*:$ROOT_DIR/tests/support/doctest.h" \
	--suppress="preprocessorErrorDirective:*vendor/libarchive/*" \
	--suppress="syntaxError:*vendor/libarchive/*" \
	--suppress="sizeofwithnumericparameter:*vendor/libarchive/*" \
	--suppress="nullPointerRedundantCheck:*vendor/libarchive/*" \
	--suppress="memleak:*vendor/libarchive/*" \
	--suppress="uninitvar:*vendor/libarchive/*" \
	--suppress="pointerSize:*vendor/libarchive/*" \
	--suppress="literalWithCharPtrCompare:*vendor/libarchive/*" \
	--suppress="internalAstError:*vendor/libarchive/*" \
	--suppress="unknownMacro:*vendor/libarchive/*" \
	--suppress="nullPointerOutOfMemory:*vendor/libarchive/*" \
	--suppress="nullPointerArithmeticRedundantCheck:*vendor/libarchive/*" \
	--suppress="resourceLeak:*vendor/libbsc/*" \
	--suppress="preprocessorErrorDirective:*vendor/libbsc/*" \
	--suppress="dangerousTypeCast:*vendor/libbsc/*" \
	--suppress="identicalInnerCondition:*vendor/libbsc/detectors.cpp" \
	--suppress="nullPointerOutOfMemory:*vendor/qvz/*" \
	--suppress="duplInheritedMember:*vendor/indexed_bzip2/*" \
	--suppress="identicalConditionAfterEarlyExit:*vendor/indexed_bzip2/src/rapidgzip/chunkdecoding/GzipChunk.hpp" \
	--suppress="sameIteratorExpression:*vendor/indexed_bzip2/src/core/FasterVector.hpp" \
	--suppress="uninitvar:*vendor/indexed_bzip2/isa-l/*" \
	--suppress="arrayIndexOutOfBoundsCond:*vendor/libdeflate/*" \
	--suppress="unknownMacro:*vendor/pthash/*" \
	--suppress="ctunullpointerOutOfMemory:*vendor/qvz/*" \
	--suppress="ctuuninitvar:*vendor/libarchive/*" \
	--suppress="invalidPrintfArgType_sint:*vendor/zstd/*" \
	--suppress="invalidPrintfArgType_uint:*vendor/zstd/*" \
	-D__BYTE_ORDER__=1 \
	-D__ORDER_LITTLE_ENDIAN__=1 \
	-I "$ROOT_DIR/src" \
	-I "$ROOT_DIR/vendor" \
	-I "$ZSTD_INCLUDE_DIR" \
	-I "$LIBBSC_INCLUDE_DIR" \
	-I "$LIBDEFLATE_INCLUDE_DIR" \
	-I "$LIBARCHIVE_INCLUDE_DIR" \
	-I "$ZLIB_INCLUDE_DIR" \
	-I "$BZIP2_INCLUDE_DIR" \
	-I "$PTHASH_INCLUDE_DIR" \
	-I "$PTHASH_EXTERNAL_DIR/xxHash" \
	-I "$PTHASH_EXTERNAL_DIR/bits/include" \
	-I "$PTHASH_EXTERNAL_DIR/bits/external/essentials/include" \
	-I "$PTHASH_EXTERNAL_DIR/mm_file/include" \
	-I "$QVZ_INCLUDE_DIR" \
	"${targets[@]}" 2>&1 | awk '/^[[:space:]]*Checking[[:space:]]+/ { next } { print }'
