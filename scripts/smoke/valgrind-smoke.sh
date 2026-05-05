#!/usr/bin/env bash

set -euo pipefail

VALGRIND_SMOKE_DIR=$(cd -- "$(dirname -- "$0")" && pwd)

# shellcheck disable=SC1091
source "$VALGRIND_SMOKE_DIR/../common/common.sh"

if [[ $# -gt 0 ]]; then
	echo "valgrind-smoke.sh does not accept file or directory targets; it only runs the Spring smoke test." >&2
	exit 1
fi

require_command valgrind
require_build_dir

VALGRIND_SUPPRESSIONS="$VALGRIND_SMOKE_DIR/valgrind.supp"
SMOKE_TEST_BIN="$BUILD_DIR/smoke-tests"

export SPRING_BIN_WRAPPER="valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds=definite,indirect --suppressions=$VALGRIND_SUPPRESSIONS --error-exitcode=1 --quiet"
export RUNNING_ON_VALGRIND=1
export BUILD_DIR

cmake --build "$BUILD_DIR" --target smoke-tests --parallel

if [[ ! -x "$SMOKE_TEST_BIN" ]]; then
	echo "Expected smoke test binary at $SMOKE_TEST_BIN" >&2
	exit 1
fi

"$SMOKE_TEST_BIN"
