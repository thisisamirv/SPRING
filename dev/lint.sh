#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

require_command python3

is_msys_windows=false
if [[ -n "${MSYSTEM:-}" ]]; then
  is_msys_windows=true
else
  case "$(uname -s)" in
    MSYS_*|MINGW*|CYGWIN*) is_msys_windows=true ;;
  esac
fi

is_macos=false
case "$(uname -s)" in
  Darwin) is_macos=true ;;
esac

clang_tidy_bin=""
for candidate in clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16; do
  if command -v "$candidate" >/dev/null 2>&1; then
    clang_tidy_bin="$candidate"
    break
  fi
done

if [[ -z "$clang_tidy_bin" ]]; then
  echo "Missing required command: clang-tidy" >&2
  exit 1
fi

GCC_INCLUDE_DIR=$(g++ -print-file-name=include)
WINDOWS_LINT_INCLUDE_DIR="$ROOT_DIR/dev/include"
MACOS_SDK_PATH=""
MACOS_CXX_INCLUDE_DIR=""

if $is_macos; then
  require_command xcrun
  MACOS_SDK_PATH="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
  apple_clangxx_path="$(xcrun --sdk macosx --find clang++)"
  apple_toolchain_usr_dir="$(cd -- "$(dirname -- "$apple_clangxx_path")/.." && pwd)"
  MACOS_CXX_INCLUDE_DIR="$apple_toolchain_usr_dir/include/c++/v1"
fi

readonly TIDY_CHECKS="*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr"
readonly ID_COMPRESSION_INCLUDE_DIR="$BUILD_DIR/vendor/id_compression/include"
readonly QVZ_INCLUDE_DIR="$BUILD_DIR/vendor/qvz/include"
readonly LIBDEFLATE_INCLUDE_DIR="$BUILD_DIR/vendor/libdeflate"
readonly EXTRA_INCLUDES=(
  "$ROOT_DIR/src"
  "$ROOT_DIR/vendor"
  "$BUILD_DIR/vendor"
  "$ID_COMPRESSION_INCLUDE_DIR"
  "$LIBDEFLATE_INCLUDE_DIR"
  "$QVZ_INCLUDE_DIR"
)

clang_tidy_common_args=(
  -quiet
  -checks="$TIDY_CHECKS"
  -header-filter='^$'
  --system-headers=false
)

if $is_msys_windows; then
  clang_tidy_common_args+=(
    --extra-arg-before=--target=x86_64-w64-windows-gnu
    --extra-arg-before=--driver-mode=g++
    --extra-arg=-w
  )
else
  clang_tidy_common_args+=(
    --extra-arg=-fopenmp
    --extra-arg=-w
    '--extra-arg=-D__malloc__(...)=__malloc__'
    --extra-arg=-I"$GCC_INCLUDE_DIR"
  )

  if $is_macos; then
    clang_tidy_common_args+=(
      --extra-arg-before=-isysroot
      --extra-arg-before="$MACOS_SDK_PATH"
      --extra-arg-before=-stdlib=libc++
    )

    if [[ -d "$MACOS_CXX_INCLUDE_DIR" ]]; then
      clang_tidy_common_args+=(
        --extra-arg-before=-isystem
        --extra-arg-before="$MACOS_CXX_INCLUDE_DIR"
      )
    fi
  fi
fi

run_clang_tidy() {
  "$clang_tidy_bin" "$@" \
    2> >(grep -Ev '^[0-9]+ warnings? generated\.$|^[0-9]+ warnings? and [0-9]+ errors? generated\.$' >&2)
}

files=()
while IFS= read -r file; do
  files+=("$file")
done < <(collect_cpp_sources "$@")

python_files=()
while IFS= read -r file; do
  python_files+=("$file")
done < <(collect_python_sources "$@")

if [[ ${#files[@]} -eq 0 && ${#python_files[@]} -eq 0 ]]; then
  echo "No C/C++ or Python source files found." >&2
  exit 1
fi

compile_db_files=()
standalone_files=()

if [[ ${#files[@]} -gt 0 ]]; then
  if $is_msys_windows; then
    standalone_files=("${files[@]}")
  else
    require_compile_commands

    for file in "${files[@]}"; do
      if compile_commands_contains "$file"; then
        compile_db_files+=("$file")
      else
        standalone_files+=("$file")
      fi
    done
  fi
fi

if [[ ${#compile_db_files[@]} -gt 0 ]]; then
  run_clang_tidy \
    "${clang_tidy_common_args[@]}" \
    -p "$BUILD_DIR" \
    "${compile_db_files[@]}"
fi

for file in "${standalone_files[@]}"; do
  include_args=()
  if $is_msys_windows; then
    include_args+=("-I$WINDOWS_LINT_INCLUDE_DIR")
  fi
  for include_dir in "${EXTRA_INCLUDES[@]}"; do
    include_args+=("-I$include_dir")
  done

  printf 'Linting standalone file %s.\n' "$file"

  run_clang_tidy \
    "${clang_tidy_common_args[@]}" \
    "$file" -- \
    -std=c++20 \
    -x c++ \
    "${include_args[@]}" \
    -I"$(dirname -- "$file")"
done

for file in "${python_files[@]}"; do
  printf 'Linting python file %s.\n' "$file"
  python3 -m py_compile "$file"
done