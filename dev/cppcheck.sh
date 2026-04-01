#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

require_command cppcheck

if [[ $# -gt 0 ]]; then
  targets=("$@")
else
  targets=("${DEFAULT_CPP_ROOTS[@]}")
fi

cppcheck \
  --error-exitcode=1 \
  --enable=warning,performance,portability \
  --suppress=missingInclude \
  --suppress=missingIncludeSystem \
  --suppress="*:$ROOT_DIR/src/BooPHF.h" \
  -I "$ROOT_DIR/src" \
  -I "$ROOT_DIR/third_party" \
  -I "$ROOT_DIR/third_party/id_compression/include" \
  -I "$ROOT_DIR/third_party/qvz/include" \
  "${targets[@]}"