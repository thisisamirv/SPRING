#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

require_command valgrind
require_spring_binary

VALGRIND_SUPPRESSIONS="$SCRIPT_DIR/valgrind.supp"

export SPRING_BIN_WRAPPER="valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible --errors-for-leak-kinds=definite,indirect --suppressions=$VALGRIND_SUPPRESSIONS --error-exitcode=1 --quiet"
"$ROOT_DIR/tests/test_script.sh"