#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
SPRING_BIN="$BUILD_DIR/spring"
COMPILE_COMMANDS="$BUILD_DIR/compile_commands.json"
readonly DEFAULT_CPP_ROOTS=("$ROOT_DIR/src" "$ROOT_DIR/experimental" "$ROOT_DIR/tests")
readonly DEFAULT_PY_ROOTS=("$ROOT_DIR/experimental" "$ROOT_DIR/tests" "$ROOT_DIR/dev")
readonly VENDOR_ROOT="$ROOT_DIR/vendor"

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

require_build_dir() {
  if [[ ! -d "$BUILD_DIR" ]]; then
    echo "Expected build directory at $BUILD_DIR" >&2
    echo "Configure SPRING first, for example: cmake -S $ROOT_DIR -B $BUILD_DIR" >&2
    exit 1
  fi
}

require_compile_commands() {
  require_build_dir
  if [[ ! -f "$COMPILE_COMMANDS" ]]; then
    echo "Expected compilation database at $COMPILE_COMMANDS" >&2
    echo "Re-run CMake so clang-tidy can use the generated compile commands." >&2
    exit 1
  fi
}

require_spring_binary() {
  if [[ ! -x "$SPRING_BIN" ]]; then
    echo "Expected built binary at $SPRING_BIN" >&2
    echo "Build SPRING first, for example: cmake --build $BUILD_DIR" >&2
    exit 1
  fi
}

is_cpp_source() {
  case "$1" in
    *.c|*.cc|*.cpp|*.cxx) return 0 ;;
    *) return 1 ;;
  esac
}

is_python_source() {
  case "$1" in
    *.py) return 0 ;;
    *) return 1 ;;
  esac
}

normalize_repo_path() {
  case "$1" in
    /*) realpath -m -- "$1" ;;
    *) realpath -m -- "$ROOT_DIR/$1" ;;
  esac
}

is_vendored_path() {
  case "$1" in
    "$VENDOR_ROOT"|"$VENDOR_ROOT"/*) return 0 ;;
    *) return 1 ;;
  esac
}

collect_first_party_paths() {
  local path

  for path in "$@"; do
    path=$(normalize_repo_path "$path")

    if is_vendored_path "$path"; then
      printf 'Skipping vendored path %s.\n' "$path" >&2
      continue
    fi

    if [[ -e "$path" ]]; then
      printf '%s\n' "$path"
    fi
  done
}

collect_cpp_sources() {
  local -a paths=()
  local path
  local file

  if [[ $# -gt 0 ]]; then
    paths=("$@")
  else
    paths=("${DEFAULT_CPP_ROOTS[@]}")
  fi

  if [[ $# -gt 0 ]]; then
    local -a filtered_paths=()
    while IFS= read -r path; do
      filtered_paths+=("$path")
    done < <(collect_first_party_paths "${paths[@]}")
    paths=("${filtered_paths[@]}")
  fi

  for path in "${paths[@]}"; do
    if [[ -d "$path" ]]; then
      while IFS= read -r file; do
        printf '%s\n' "$file"
      done < <(find "$path" -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) | sort)
      continue
    fi

    if [[ -f "$path" ]] && is_cpp_source "$path"; then
      printf '%s\n' "$path"
    fi
  done
}

collect_python_sources() {
  local -a paths=()
  local path
  local file

  if [[ $# -gt 0 ]]; then
    paths=("$@")
  else
    paths=("${DEFAULT_PY_ROOTS[@]}")
  fi

  if [[ $# -gt 0 ]]; then
    local -a filtered_paths=()
    while IFS= read -r path; do
      filtered_paths+=("$path")
    done < <(collect_first_party_paths "${paths[@]}")
    paths=("${filtered_paths[@]}")
  fi

  for path in "${paths[@]}"; do
    if [[ -d "$path" ]]; then
      while IFS= read -r file; do
        printf '%s\n' "$file"
      done < <(find "$path" -type f -name '*.py' | sort)
      continue
    fi

    if [[ -f "$path" ]] && is_python_source "$path"; then
      printf '%s\n' "$path"
    fi
  done
}

compile_commands_contains() {
  local path="$1"
  if grep -F -- "\"file\": \"$path\"" "$COMPILE_COMMANDS" >/dev/null 2>&1; then
    return 0
  fi

  if command -v cygpath >/dev/null 2>&1; then
    local windows_path
    windows_path=$(cygpath -m -- "$path")
    grep -F -- "\"file\": \"$windows_path\"" "$COMPILE_COMMANDS" >/dev/null 2>&1
    return $?
  fi

  return 1
}