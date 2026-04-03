#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

require_command cppcheck

ID_COMPRESSION_INCLUDE_DIR="$BUILD_DIR/vendor/id_compression/include"
QVZ_INCLUDE_DIR="$BUILD_DIR/vendor/qvz/include"

if [[ $# -gt 0 ]]; then
  targets=()
  while IFS= read -r target; do
    targets+=("$target")
  done < <(collect_first_party_paths "$@")
else
  targets=("${DEFAULT_CPP_ROOTS[@]}")
fi

if [[ ${#targets[@]} -eq 0 ]]; then
  echo "No first-party targets selected for cppcheck." >&2
  exit 1
fi

cppcheck \
  --error-exitcode=1 \
  --enable=warning,performance,portability \
  --suppress=missingInclude \
  --suppress=missingIncludeSystem \
  --suppress="*:$ROOT_DIR/src/BooPHF.h" \
  -I "$ROOT_DIR/src" \
  -I "$ROOT_DIR/vendor" \
  -I "$ID_COMPRESSION_INCLUDE_DIR" \
  -I "$QVZ_INCLUDE_DIR" \
  "${targets[@]}"