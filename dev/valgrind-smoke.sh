#!/usr/bin/env bash

set -euo pipefail

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

if [[ $# -gt 0 ]]; then
	echo "valgrind-smoke.sh does not accept file or directory targets; it only runs the Spring smoke test." >&2
	exit 1
fi

require_command valgrind
require_spring_binary

VALGRIND_SUPPRESSIONS="$SCRIPT_DIR/valgrind.supp"

export SPRING_BIN_WRAPPER="valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds=definite,indirect --suppressions=$VALGRIND_SUPPRESSIONS --error-exitcode=1 --quiet"
export RUNNING_ON_VALGRIND=1
"$ROOT_DIR/tests/smoke_test.sh"
