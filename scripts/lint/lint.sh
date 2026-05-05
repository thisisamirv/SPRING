#!/usr/bin/env bash

set -euo pipefail

# shellcheck disable=SC1091
source "$(cd -- "$(dirname -- "$0")" && pwd)/../common/common.sh"
require_command python3

is_msys_windows=false
if [[ -n "${MSYSTEM:-}" ]]; then
	is_msys_windows=true
else
	case "$(uname -s)" in
	MSYS_* | MINGW* | CYGWIN*) is_msys_windows=true ;;
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

LINT_INCLUDE_DIR="$ROOT_DIR/scripts/lint/include"
MACOS_SDK_PATH=""
MACOS_CXX_INCLUDE_DIR=""
LINUX_GCC_TOOLCHAIN_DIR=""
LINUX_SYSTEM_INCLUDE_DIRS=()
LINUX_CLANG_RESOURCE_INCLUDE_DIR=""

if $is_macos; then
	require_command xcrun
	MACOS_SDK_PATH="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
	apple_clangxx_path="$(xcrun --sdk macosx --find clang++)"
	apple_toolchain_usr_dir="$(cd -- "$(dirname -- "$apple_clangxx_path")/.." && pwd)"
	MACOS_CXX_INCLUDE_DIR="$apple_toolchain_usr_dir/include/c++/v1"
else
	LINUX_GCC_TOOLCHAIN_DIR="$(dirname -- "$(dirname -- "$(g++ -print-libgcc-file-name)")")"
	clangxx_bin=""
	for candidate in clang++ clang++-18 clang++-17 clang++-16; do
		if command -v "$candidate" >/dev/null 2>&1; then
			clangxx_bin="$candidate"
			break
		fi
	done
	if [[ -n "$clangxx_bin" ]]; then
		LINUX_CLANG_RESOURCE_INCLUDE_DIR="$("$clangxx_bin" -print-resource-dir)/include"
	fi
	while IFS= read -r include_dir; do
		if [[ -n "$include_dir" && ! "$include_dir" =~ /lib/gcc/.*/include$ ]]; then
			LINUX_SYSTEM_INCLUDE_DIRS+=("$include_dir")
		fi
	done < <(
		g++ -E -x c++ - -v </dev/null 2>&1 |
			awk '/#include <...> search starts here:/{capture=1; next} /End of search list\./{capture=0} capture { sub(/^ +/, ""); print }'
	)
fi

readonly TIDY_CHECKS="*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr,-clang-analyzer-cplusplus.NewDeleteLeaks,-misc-use-anonymous-namespace,-cppcoreguidelines-pro-type-union-access,-bugprone-macro-parentheses,-bugprone-implicit-widening-of-multiplication-result,-readability-avoid-nested-conditional-operator,-hicpp-deprecated-headers,-modernize-deprecated-headers,-misc-redundant-expression,-cppcoreguidelines-avoid-goto,-hicpp-avoid-goto,-modernize-redundant-void-arg,-readability-redundant-casting,-readability-inconsistent-declaration-parameter-name,-clang-analyzer-core.BitwiseShift,-bugprone-casting-through-void,-cppcoreguidelines-use-enum-class,-performance-enum-size,-readability-uppercase-literal-suffix,-readability-redundant-parentheses,-bugprone-assignment-in-if-condition,-modernize-use-bool-literals,-bugprone-inc-dec-in-conditions,-clang-analyzer-core.NullPointerArithm,-hicpp-function-size,-readability-function-size,-clang-analyzer-deadcode.DeadStores,-clang-analyzer-core.uninitialized.Assign,-bugprone-reserved-identifier,-performance-no-int-to-ptr,-bugprone-suspicious-string-compare,-hicpp-multiway-paths-covered,-readability-redundant-member-init,-readability-container-contains,-misc-no-recursion,-readability-duplicate-include,-bugprone-signed-char-misuse,-modernize-avoid-variadic-functions,-bugprone-multi-level-implicit-pointer-conversion,-readability-avoid-unconditional-preprocessor-if,-clang-analyzer-core.UndefinedBinaryOperatorResult,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.strcpy,-readability-redundant-preprocessor,-misc-confusable-identifiers,-modernize-use-designated-initializers,-portability-simd-intrinsics,-modernize-use-integer-sign-comparison,-misc-unused-parameters,-readability-suspicious-call-argument,-hicpp-no-assembler,-readability-avoid-return-with-void-value,-clang-analyzer-unix.Stream,-modernize-concat-nested-namespaces,-readability-convert-member-functions-to-static,-bugprone-unchecked-string-to-number-conversion,-performance-inefficient-vector-operation,-clang-analyzer-core.NullDereference,-portability-avoid-pragma-once,-cppcoreguidelines-non-private-member-variables-in-classes,-cppcoreguidelines-special-member-functions,-hicpp-special-member-functions,-readability-redundant-string-init,-modernize-use-nodiscard,-cppcoreguidelines-use-default-member-init,-modernize-use-default-member-init,-readability-const-return-type,-cppcoreguidelines-avoid-const-or-ref-data-members,-performance-unnecessary-value-param,-misc-anonymous-namespace-in-header,-readability-qualified-auto,-hicpp-use-emplace,-modernize-use-emplace,-boost-use-ranges,-modernize-use-ranges,-readability-avoid-const-params-in-decls,-readability-redundant-access-specifiers,-readability-redundant-typename,-bugprone-throwing-static-initialization,-cppcoreguidelines-pro-type-const-cast,-bugprone-unintended-char-ostream-output,-modernize-use-constraints,-misc-override-with-different-visibility,-bugprone-derived-method-shadowing-base-method,-bugprone-unchecked-optional-access,-cppcoreguidelines-rvalue-reference-param-not-moved,-clang-analyzer-optin.portability.UnixAPI,-bugprone-suspicious-stringview-data-usage,-clang-analyzer-unix.StdCLibraryFunctions,-hicpp-exception-baseclass,-misc-throw-by-value-catch-by-reference,-bugprone-string-literal-with-embedded-nul,-modernize-use-scoped-lock,-cppcoreguidelines-misleading-capture-default-by-value,-cppcoreguidelines-pro-type-static-cast-downcast,-android-cloexec-dup,-misc-multiple-inheritance,-readability-use-anyofallof,-modernize-type-traits,-cppcoreguidelines-missing-std-forward,-cppcoreguidelines-explicit-virtual-functions,-hicpp-use-override,-modernize-use-override,-modernize-use-std-numbers,-clang-analyzer-valist.Uninitialized,-bugprone-non-zero-enum-to-bool-conversion,-bugprone-sizeof-expression,-bugprone-not-null-terminated-result"
ZSTD_INCLUDE_DIR="$ROOT_DIR/vendor/zstd"
LIBBSC_INCLUDE_DIR="$ROOT_DIR/vendor/libbsc"
LIBDEFLATE_INCLUDE_DIR="$ROOT_DIR/vendor/libdeflate"
LIBARCHIVE_INCLUDE_DIR="$ROOT_DIR/vendor/libarchive/lib"
ZLIB_INCLUDE_DIR="$ROOT_DIR/vendor/cloudflare_zlib"
BZIP2_INCLUDE_DIR="$ROOT_DIR/vendor/indexed_bzip2"
BZIP2_ISAL_INCLUDE_DIR="$ROOT_DIR/vendor/indexed_bzip2/isa-l/include"
QVZ_INCLUDE_DIR="$ROOT_DIR/vendor/qvz"
PTHASH_INCLUDE_DIR="$ROOT_DIR/vendor/pthash"
readonly EXTRA_INCLUDES=(
	"$ROOT_DIR/src"
	"$ROOT_DIR/src/common"
	"$ROOT_DIR/src/assays"
	"$ROOT_DIR/src/decompress"
	"$ROOT_DIR/src/encode"
	"$ROOT_DIR/src/preprocess"
	"$ROOT_DIR/src/reorder"
	"$ROOT_DIR/src/workflow"
	"$ROOT_DIR/vendor"
	"$ZSTD_INCLUDE_DIR"
	"$LIBBSC_INCLUDE_DIR"
	"$LIBDEFLATE_INCLUDE_DIR"
	"$LIBARCHIVE_INCLUDE_DIR"
	"$ZLIB_INCLUDE_DIR"
	"$BZIP2_INCLUDE_DIR"
	"$BZIP2_ISAL_INCLUDE_DIR"
	"$QVZ_INCLUDE_DIR"
	"$PTHASH_INCLUDE_DIR"
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
		--extra-arg=-I"$LINT_INCLUDE_DIR"
		--extra-arg=-fopenmp
		--extra-arg=-w
		--extra-arg=-fconstexpr-steps=4194304
	)
else
	if $is_macos; then
		clang_tidy_common_args+=(
			--extra-arg=-w
			--extra-arg=-fconstexpr-steps=4194304
		)

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
	else
		clang_tidy_common_args+=(
			--extra-arg-before=--target=x86_64-linux-gnu
			--extra-arg-before=--gcc-toolchain="$LINUX_GCC_TOOLCHAIN_DIR"
			--extra-arg=-I"$LINT_INCLUDE_DIR"
			--extra-arg=-isystem
			--extra-arg="$ROOT_DIR/tests/support"
			--extra-arg=-fopenmp
			--extra-arg=-w
			--extra-arg=-fconstexpr-steps=4194304
			'--extra-arg=-D__malloc__(...)=__malloc__'
		)

		if [[ -n "$LINUX_CLANG_RESOURCE_INCLUDE_DIR" ]]; then
			clang_tidy_common_args+=(
				--extra-arg-before=-isystem
				--extra-arg-before="$LINUX_CLANG_RESOURCE_INCLUDE_DIR"
			)
		fi

		for include_dir in "${LINUX_SYSTEM_INCLUDE_DIRS[@]}"; do
			clang_tidy_common_args+=(
				--extra-arg-before=-isystem
				--extra-arg-before="$include_dir"
			)
		done
	fi
fi

run_clang_tidy() {
	"$clang_tidy_bin" "$@" \
		2> >(grep -Ev '^[0-9]+ warnings? generated\.$|^[0-9]+ warnings? and [0-9]+ errors? generated\.$' >&2)
}

run_parallel_lint_jobs() {
	local job_count="$1"
	local worker_function="$2"
	shift 2
	local -a work_items=("$@")
	local overall_status=0
	local start_index=0

	if [[ ${#work_items[@]} -eq 0 ]]; then
		return 0
	fi

	if ((job_count < 2 || ${#work_items[@]} < 2)); then
		for work_item in "${work_items[@]}"; do
			if ! "$worker_function" "$work_item"; then
				overall_status=1
			fi
		done
		return "$overall_status"
	fi

	while ((start_index < ${#work_items[@]})); do
		local batch_end=$((start_index + job_count))
		local -a batch_pids=()
		local -a batch_outputs=()
		local batch_index

		if ((batch_end > ${#work_items[@]})); then
			batch_end=${#work_items[@]}
		fi

		for ((batch_index = start_index; batch_index < batch_end; batch_index++)); do
			local output_file
			output_file=$(mktemp "${TMPDIR:-/tmp}/spring2-lint.XXXXXX")
			batch_outputs+=("$output_file")
			(
				"$worker_function" "${work_items[batch_index]}"
			) >"$output_file" 2>&1 &
			batch_pids+=("$!")
		done

		for batch_pid in "${batch_pids[@]}"; do
			if ! wait "$batch_pid"; then
				overall_status=1
			fi
		done

		for output_file in "${batch_outputs[@]}"; do
			cat "$output_file"
			rm -f "$output_file"
		done

		start_index=$batch_end
	done

	return "$overall_status"
}

run_compile_db_tidy_for_file() {
	local file="$1"
	printf 'Linting compile-db file %s.\n' "$file"
	run_clang_tidy \
		"${clang_tidy_common_args[@]}" \
		-p "$tidy_db_dir" \
		"$file"
}

run_standalone_tidy_for_file() {
	local file="$1"
	local -a include_args=()

	if $is_msys_windows; then
		include_args+=("-I$LINT_INCLUDE_DIR")
	fi
	for include_dir in "${EXTRA_INCLUDES[@]}"; do
		include_args+=("-I$include_dir")
	done

	printf 'Linting standalone file %s.\n' "$file"

	if [[ "$file" == *.c ]]; then
		run_clang_tidy \
			"${clang_tidy_common_args[@]}" \
			"$file" -- \
			-std=gnu11 \
			-x c \
			"${include_args[@]}" \
			-I"$(dirname -- "$file")"
	else
		run_clang_tidy \
			"${clang_tidy_common_args[@]}" \
			"$file" -- \
			--driver-mode=g++ \
			-std=c++20 \
			-x c++ \
			"${include_args[@]}" \
			-I"$(dirname -- "$file")"
	fi
}

run_python_lint_for_file() {
	local file="$1"
	printf 'Linting python file %s.\n' "$file"
	python3 -m py_compile "$file"
}

lint_jobs=$(resolve_parallel_job_count)
printf 'Using up to %s parallel lint jobs.\n' "$lint_jobs"

if [[ $# -gt 0 ]]; then
	lint_targets=("$@")
else
	# Do not lint the tests/ directory by default
	lint_targets=("$ROOT_DIR/src" "$ROOT_DIR/vendor")
fi

files=()
while IFS= read -r file; do
	if [[ "$file" == *"doctest.h" ]]; then
		continue
	fi
	files+=("$file")
done < <(collect_cpp_sources "${lint_targets[@]}")

python_files=()
while IFS= read -r file; do
	python_files+=("$file")
done < <(collect_python_sources "${lint_targets[@]}")

if [[ ${#files[@]} -eq 0 && ${#python_files[@]} -eq 0 ]]; then
	echo "No C/C++ or Python source files found to lint."
	exit 0
fi

tidy_db_dir="$BUILD_DIR"

sanitize_compile_commands_for_tidy() {
	local source_db="$1"
	local target_db="$2"

	python3 - "$source_db" "$target_db" <<'PY'
import json
import pathlib
import shlex
import sys


def _is_pch_path(arg: str) -> bool:
	lower = arg.lower()
	return "cmake_pch.h" in lower or "cmake_pch.hxx" in lower


def _sanitize_args(args):
	sanitized = []
	i = 0
	while i < len(args):
		arg = args[i]

		if arg in ("-include", "-include-pch") and i + 1 < len(args):
			if _is_pch_path(args[i + 1]):
				i += 2
				continue

		# Clang sometimes emits PCH args as:
		#   -Xclang -include-pch -Xclang /path/to/cmake_pch.hxx.pch
		if arg == "-Xclang" and i + 1 < len(args):
			next_arg = args[i + 1]
			if next_arg in ("-include", "-include-pch"):
				if i + 3 < len(args) and args[i + 2] == "-Xclang" and _is_pch_path(args[i + 3]):
					i += 4
					continue
				i += 2
				continue
			if _is_pch_path(next_arg):
				i += 2
				continue

		if arg.startswith("-include-pch") and _is_pch_path(arg):
			i += 1
			continue

		if arg.startswith("-include") and _is_pch_path(arg):
			i += 1
			continue

		if _is_pch_path(arg):
			i += 1
			continue

		sanitized.append(arg)
		i += 1
	return sanitized


src_path = pathlib.Path(sys.argv[1])
dst_path = pathlib.Path(sys.argv[2])
entries = json.loads(src_path.read_text(encoding="utf-8"))

for entry in entries:
	if "arguments" in entry and isinstance(entry["arguments"], list):
		entry["arguments"] = _sanitize_args(entry["arguments"])
		continue

	command = entry.get("command")
	if isinstance(command, str) and command.strip():
		try:
			split = shlex.split(command)
			entry["command"] = shlex.join(_sanitize_args(split))
		except ValueError:
			# Keep original command when shell parsing fails.
			pass

dst_path.parent.mkdir(parents=True, exist_ok=True)
dst_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
PY
}

compile_db_files=()
standalone_files=()

if [[ ${#files[@]} -gt 0 ]]; then
	require_build_dir

	if [[ ! -f "$COMPILE_COMMANDS" ]]; then
		echo "compile_commands.json not found; re-running CMake configure with export enabled..." >&2
		cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	fi

	if [[ ! -f "$COMPILE_COMMANDS" ]]; then
		echo "Expected compilation database at $COMPILE_COMMANDS" >&2
		exit 1
	fi

	tidy_db_dir="$BUILD_DIR/tidy_db"
	tidy_compile_commands="$tidy_db_dir/compile_commands.json"
	echo "Sanitizing compilation database for clang-tidy..."
	sanitize_compile_commands_for_tidy "$COMPILE_COMMANDS" "$tidy_compile_commands"
	COMPILE_COMMANDS="$tidy_compile_commands"
	# shellcheck disable=SC2034
	COMPILE_COMMANDS_FILE_SET_INITIALIZED=false

	for file in "${files[@]}"; do
		if compile_commands_contains "$file"; then
			compile_db_files+=("$file")
		else
			standalone_files+=("$file")
		fi
	done

	if [[ ${#compile_db_files[@]} -eq 0 && ${#standalone_files[@]} -gt 0 ]]; then
		echo "No files matched compile_commands.json directly; falling back to compilation-database mode for all files." >&2
		compile_db_files=("${standalone_files[@]}")
		standalone_files=()
	fi
fi

if [[ -n "${compile_db_files+set}" && ${#compile_db_files[@]} -gt 0 ]]; then
	run_parallel_lint_jobs "$lint_jobs" run_compile_db_tidy_for_file "${compile_db_files[@]}"
fi

if [[ -n "${standalone_files+set}" && ${#standalone_files[@]} -gt 0 ]]; then
	run_parallel_lint_jobs "$lint_jobs" run_standalone_tidy_for_file "${standalone_files[@]}"
fi

if [[ -n "${python_files+set}" && ${#python_files[@]} -gt 0 ]]; then
	run_parallel_lint_jobs "$lint_jobs" run_python_lint_for_file "${python_files[@]}"
fi
