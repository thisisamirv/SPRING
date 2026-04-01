#!/usr/bin/env bash

set -euo pipefail

source "$(cd -- "$(dirname -- "$0")" && pwd)/common.sh"

require_command clang-tidy
require_command python3

GCC_INCLUDE_DIR=$(g++ -print-file-name=include)

readonly TIDY_CHECKS="*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr"
readonly EXTRA_INCLUDES=(
  "$ROOT_DIR/src"
  "$ROOT_DIR/third_party"
  "$ROOT_DIR/third_party/id_compression/include"
  "$ROOT_DIR/third_party/qvz/include"
)

clang_tidy_common_args=(
  -quiet
  -checks="$TIDY_CHECKS"
  -header-filter='^$'
  --system-headers=false
  --extra-arg=-fopenmp
  --extra-arg=-I"$GCC_INCLUDE_DIR"
)

mapfile -t files < <(collect_cpp_sources "$@")
mapfile -t python_files < <(collect_python_sources "$@")

if [[ ${#files[@]} -eq 0 && ${#python_files[@]} -eq 0 ]]; then
  echo "No C/C++ or Python source files found." >&2
  exit 1
fi

compile_db_files=()
standalone_files=()

if [[ ${#files[@]} -gt 0 ]]; then
  require_compile_commands

  for file in "${files[@]}"; do
    if compile_commands_contains "$file"; then
      compile_db_files+=("$file")
    else
      standalone_files+=("$file")
    fi
  done
fi

if [[ ${#compile_db_files[@]} -gt 0 ]]; then
  clang-tidy \
    "${clang_tidy_common_args[@]}" \
    -p "$BUILD_DIR" \
    "${compile_db_files[@]}"
fi

for file in "${standalone_files[@]}"; do
  include_args=()
  for include_dir in "${EXTRA_INCLUDES[@]}"; do
    include_args+=("-I$include_dir")
  done

  printf 'Linting standalone file %s.\n' "$file"

  clang-tidy \
    "${clang_tidy_common_args[@]}" \
    "$file" -- \
    -std=c++11 \
    -x c++ \
    -fopenmp \
    "${include_args[@]}" \
    -I"$(dirname -- "$file")" \
    -I"$GCC_INCLUDE_DIR"
done

for file in "${python_files[@]}"; do
  printf 'Linting python file %s.\n' "$file"
  python3 -m py_compile "$file"
done