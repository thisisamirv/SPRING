#!/bin/bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
ASSET_DIR="$ROOT_DIR/assets/sample-data"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

usage() {
	echo "Usage: $0 [--build <path>]"
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--build|-b)
			if [[ $# -lt 2 ]]; then
				echo "Missing value for $1" >&2
				usage >&2
				exit 1
			fi
			BUILD_DIR="$2"
			shift 2
			;;
		--build=*)
			BUILD_DIR="${1#*=}"
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage >&2
			exit 1
			;;
	esac
done

if [[ "$BUILD_DIR" != /* ]]; then
	BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
fi
SPRING_BIN="${SPRING_BIN:-$BUILD_DIR/spring2}"
SPRING_PREVIEW_BIN="${SPRING_PREVIEW_BIN:-$BUILD_DIR/spring2-preview}"
SPRING_BIN_CMD=()
SPRING_PREVIEW_BIN_CMD=()
SPRING_TEST_ARGS_CMD=()
SPRING_COMMAND_TIMEOUT_SECONDS="${SPRING_COMMAND_TIMEOUT_SECONDS:-0}"
SMOKE_THREADS_COMPRESS="${SMOKE_THREADS_COMPRESS:-8}"
SMOKE_THREADS_DECOMPRESS="${SMOKE_THREADS_DECOMPRESS:-5}"

detect_cpu_threads() {
	local detected=""
	if command -v nproc >/dev/null 2>&1; then
		detected=$(nproc 2>/dev/null || true)
	elif command -v getconf >/dev/null 2>&1; then
		detected=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
	elif command -v sysctl >/dev/null 2>&1; then
		detected=$(sysctl -n hw.ncpu 2>/dev/null || true)
	fi

	if [[ ! "$detected" =~ ^[0-9]+$ ]] || [[ "$detected" -lt 1 ]]; then
		detected=1
	fi
	echo "$detected"
}

cap_threads() {
	local requested="$1"
	local max_threads="$2"
	if [[ ! "$requested" =~ ^[0-9]+$ ]] || [[ "$requested" -lt 1 ]]; then
		requested=1
	fi
	if [[ "$requested" -gt "$max_threads" ]]; then
		echo "$max_threads"
	else
		echo "$requested"
	fi
}

CPU_THREADS=$(detect_cpu_threads)
SMOKE_THREADS_COMPRESS=$(cap_threads "$SMOKE_THREADS_COMPRESS" "$CPU_THREADS")
SMOKE_THREADS_DECOMPRESS=$(cap_threads "$SMOKE_THREADS_DECOMPRESS" "$CPU_THREADS")
mkdir -p "$ROOT_DIR/tests/output"
WORK_DIR=$(mktemp -d "$ROOT_DIR/tests/output/smoke-test.XXXXXX")
CURRENT_SMOKE_CASE=""

dump_system_diagnostics() {
	echo "Smoke diagnostics: system" >&2
	if command -v uname >/dev/null 2>&1; then
		echo "Smoke diagnostics: uname=$(uname -a)" >&2
	fi
	if [[ -r /etc/os-release ]]; then
		echo "Smoke diagnostics: os-release" >&2
		sed -n '1,20p' /etc/os-release >&2 || true
	fi
	if command -v lscpu >/dev/null 2>&1; then
		echo "Smoke diagnostics: cpu summary" >&2
		lscpu | sed -n '1,15p' >&2 || true
	fi
	echo "Smoke diagnostics: cwd=$PWD" >&2
	echo "Smoke diagnostics: PATH=$PATH" >&2

	echo "Smoke diagnostics: selected environment" >&2
	env | grep -E '^(CI|GITHUB_|RUNNER_|SPRING_|OMP_|CC=|CXX=|PATH=|SHELL=|LANG=|LC_)' | sort >&2 || true

	echo "Smoke diagnostics: tool versions" >&2
	for tool in bash cmake ninja gcc g++ clang ld mold lld tar gzip xz python3; do
		if command -v "$tool" >/dev/null 2>&1; then
			version_line=$($tool --version 2>/dev/null | head -n 1)
			echo "  $tool: $version_line" >&2
		else
			echo "  $tool: not found" >&2
		fi
	done

	if [[ -x "$SPRING_BIN" ]]; then
		spring_version=$($SPRING_BIN --version 2>&1 | head -n 1)
		echo "  spring2: $spring_version" >&2
	fi
	if [[ -x "$SPRING_PREVIEW_BIN" ]]; then
		preview_version=$($SPRING_PREVIEW_BIN --version 2>&1 | head -n 1)
		echo "  spring2-preview: $preview_version" >&2
	fi
}

dump_debug_state() {
	local exit_code="$1"
	echo "Smoke debug: case=${CURRENT_SMOKE_CASE:-unknown} exit_code=$exit_code" >&2
	echo "Smoke debug: pwd=$PWD" >&2
	if [[ -d "$PWD" ]]; then
		ls -la >&2 || true
	fi
	for candidate in win-single win-paired win-gzip win-single-out win-paired-out.1 win-paired-out.2 win-gzip-out abcd tmp tmp.1 tmp.2; do
		if [[ -e "$candidate" ]]; then
			echo "Smoke debug: found $candidate" >&2
			ls -l "$candidate" >&2 || true
		fi
	done
	if command -v tar >/dev/null 2>&1; then
		for archive in win-single win-paired win-gzip abcd; do
			if [[ -f "$archive" ]]; then
				echo "Smoke debug: tar contents for $archive" >&2
				tar -tf "$archive" >&2 || true
			fi
		done
	fi
}

on_error() {
	local exit_code="$?"
	dump_debug_state "$exit_code"
	dump_system_diagnostics
	exit "$exit_code"
}

cleanup() {
	rm -rf "$WORK_DIR"
}

trap cleanup EXIT
trap on_error ERR

ensure_smoke_binaries() {
	if ! command -v cmake >/dev/null 2>&1; then
		echo "cmake is required to build smoke-test binaries" >&2
		exit 1
	fi

	if [[ ! -d "$BUILD_DIR" ]]; then
		echo "Build directory not found: $BUILD_DIR" >&2
		exit 1
	fi

	echo "Building smoke binaries (spring2, spring2-preview)..."
	cmake --build "$BUILD_DIR" --target spring2 spring2-preview --parallel
}

ensure_smoke_binaries

if [[ ! -x "$SPRING_BIN" ]]; then
	echo "Expected built binary at $SPRING_BIN"
	exit 1
fi

if [[ ! -x "$SPRING_PREVIEW_BIN" ]]; then
	echo "Expected built binary at $SPRING_PREVIEW_BIN"
	exit 1
fi

if [[ -n "${SPRING_BIN_WRAPPER:-}" ]]; then
	# Split a simple wrapper command such as valgrind into argv form.
	# shellcheck disable=SC2206
	SPRING_BIN_CMD=(${SPRING_BIN_WRAPPER})
	SPRING_BIN_CMD+=("$SPRING_BIN")
	# shellcheck disable=SC2206
	SPRING_PREVIEW_BIN_CMD=(${SPRING_BIN_WRAPPER})
	SPRING_PREVIEW_BIN_CMD+=("$SPRING_PREVIEW_BIN")
else
	SPRING_BIN_CMD=("$SPRING_BIN")
	SPRING_PREVIEW_BIN_CMD=("$SPRING_PREVIEW_BIN")
fi

if [[ -n "${SPRING_TEST_ARGS:-}" ]]; then
	# Split simple extra Spring CLI arguments from the environment.
	# shellcheck disable=SC2206
	SPRING_TEST_ARGS_CMD=(${SPRING_TEST_ARGS})
fi

run_spring() {
	local cmd=("${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" "$@")
	local has_verbose=0
	for arg in "${cmd[@]}"; do
		if [[ "$arg" == "-v" || "$arg" == "--verbose" ]]; then
			has_verbose=1
			break
		fi
	done
	if [[ "$has_verbose" -eq 0 ]]; then
		cmd=("${SPRING_BIN_CMD[@]}" -v debug "${SPRING_TEST_ARGS_CMD[@]}" "$@")
	fi
	local is_compress=0
	for arg in "${cmd[@]}"; do
		if [[ "$arg" == "-c" || "$arg" == "--compress" ]]; then
			is_compress=1
			break
		fi
	done
	if [[ "$is_compress" -eq 1 ]]; then
		local output_path=""
		for ((i = 0; i < ${#cmd[@]} - 1; ++i)); do
			if [[ "${cmd[$i]}" == "-o" || "${cmd[$i]}" == "--output" ]]; then
				output_path="${cmd[$((i + 1))]}"
				break
			fi
		done
		if [[ -n "$output_path" && -e "$output_path" ]]; then
			rm -f -- "$output_path"
		fi
	fi

	local display_cmd=()
	for arg in "${cmd[@]}"; do
		display_cmd+=("$(printf '%q' "$arg")")
	done
	if [[ "$SPRING_COMMAND_TIMEOUT_SECONDS" -gt 0 ]] && command -v timeout >/dev/null 2>&1; then
		echo "Smoke command: timeout ${SPRING_COMMAND_TIMEOUT_SECONDS} ${display_cmd[*]}"
		timeout "$SPRING_COMMAND_TIMEOUT_SECONDS" "${cmd[@]}"
	else
		echo "Smoke command: ${display_cmd[*]}"
		"${cmd[@]}"
	fi
}

announce_case() {
	CURRENT_SMOKE_CASE="$1"
	echo "Smoke case: $1"
}

compare_files() {
	local left_path="$1"
	local right_path="$2"
	if command -v cmp >/dev/null 2>&1; then
		if ! cmp "$left_path" "$right_path"; then
			echo "DEBUG MISMATCH: $left_path vs $right_path"
			head -n 1 "$left_path" | cat -A
			echo "---"
			head -n 1 "$right_path" | cat -A
		fi
		cmp "$left_path" "$right_path"
		return
	fi
	if command -v diff >/dev/null 2>&1; then
		diff -q "$left_path" "$right_path" >/dev/null
		return
	fi
	if [[ "$(<"$left_path")" != "$(<"$right_path")" ]]; then
		echo "Files differ: $left_path $right_path" >&2
		return 1
	fi
}

compare_lines() {
	local left_path="$1"
	local right_path="$2"
	local left_lines
	local right_lines
	left_lines=$(wc -l < "$left_path")
	right_lines=$(wc -l < "$right_path")
	if [[ "$left_lines" != "$right_lines" ]]; then
		echo "Line count differ: $left_path ($left_lines) vs $right_path ($right_lines)" >&2
		return 1
	fi
}

compare_with_gzip_source() {
	local output_path="$1"
	local gzip_source_path="$2"
	local expected_path
	expected_path=$(mktemp "$WORK_DIR/expected-from-gz.XXXXXX")
	gzip -dc "$gzip_source_path" >"$expected_path"
	compare_files "$output_path" "$expected_path"
	rm -f "$expected_path"
}

prepare_local_input() {
	local source_path="$1"
	local target_name="$2"
	cp "$source_path" "$target_name"
}

cd "$WORK_DIR"

announce_case "fastq round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "fasta round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

announce_case "paired fastq round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fasta round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fasta" --R2 "$ASSET_DIR/test_2.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fasta"
compare_files tmp.2 "$ASSET_DIR/test_2.fasta"

announce_case "paired reads + single index lane round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" --I1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.R1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.R2 "$ASSET_DIR/test_2.fastq"
compare_files tmp.I1 "$ASSET_DIR/test_1.fastq"

announce_case "paired reads + paired index lanes round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" --I1 "$ASSET_DIR/test_1.fastq" --I2 "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.R1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.R2 "$ASSET_DIR/test_2.fastq"
compare_files tmp.I1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.I2 "$ASSET_DIR/test_2.fastq"

announce_case "paired reads + R3 round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" --R3 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.R1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.R2 "$ASSET_DIR/test_2.fastq"
compare_files tmp.R3 "$ASSET_DIR/test_1.fastq"

announce_case "paired reads + R3 + paired index lanes round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" --R3 "$ASSET_DIR/test_1.fastq" --I1 "$ASSET_DIR/test_1.fastq" --I2 "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.R1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.R2 "$ASSET_DIR/test_2.fastq"
compare_files tmp.R3 "$ASSET_DIR/test_1.fastq"
compare_files tmp.I1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.I2 "$ASSET_DIR/test_2.fastq"

announce_case "fastq to gzipped output"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"
run_spring -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "fasta round-trip repeat"
run_spring -c --R1 "$ASSET_DIR/test_1.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

announce_case "paired fastq round-trip repeat"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fastq gzipped input round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq.gz" --R2 "$ASSET_DIR/test_2.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_with_gzip_source tmp.1 "$ASSET_DIR/test_1.fastq.gz"
compare_with_gzip_source tmp.2 "$ASSET_DIR/test_2.fastq.gz"

announce_case "paired fasta gzipped input round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fasta.gz" --R2 "$ASSET_DIR/test_2.fasta.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_with_gzip_source tmp.1 "$ASSET_DIR/test_1.fasta.gz"
compare_with_gzip_source tmp.2 "$ASSET_DIR/test_2.fasta.gz"

announce_case "single gzipped fastq round-trip"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_with_gzip_source tmp "$ASSET_DIR/test_1.fastq.gz"

announce_case "single gzipped fastq to gzipped output"
run_spring -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_with_gzip_source tmp "$ASSET_DIR/test_1.fastq.gz"

announce_case "paired fastq gzipped input round-trip redundant"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq.gz" --R2 "$ASSET_DIR/test_2.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_with_gzip_source tmp.1 "$ASSET_DIR/test_1.fastq.gz"
compare_with_gzip_source tmp.2 "$ASSET_DIR/test_2.fastq.gz"

announce_case "paired fastq gzipped input to gzipped outputs"
run_spring -d -i abcd -o tmp.1.gz tmp.2.gz
gunzip -f tmp.1.gz
gunzip -f tmp.2.gz
compare_with_gzip_source tmp.1 "$ASSET_DIR/test_1.fastq.gz"
compare_with_gzip_source tmp.2 "$ASSET_DIR/test_2.fastq.gz"

announce_case "multi-threaded round-trip single"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" -o abcd -t "$SMOKE_THREADS_COMPRESS"
run_spring -d -i abcd -o tmp -t "$SMOKE_THREADS_DECOMPRESS"
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "multi-threaded round-trip paired"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" -o abcd -t "$SMOKE_THREADS_COMPRESS"
run_spring -d -i abcd -o tmp -t "$SMOKE_THREADS_DECOMPRESS"
if ! cmp tmp.1 "$ASSET_DIR/test_1.fastq"; then
	echo "MISMATCH FOUND"
	head -c 20 tmp.1 | cat -A
	echo "EXPECTED:"
	head -c 20 "$ASSET_DIR/test_1.fastq" | cat -A
fi
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "sorted output round-trip single"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" -o abcd -s o
run_spring -d -i abcd -o tmp
sort tmp >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

announce_case "sorted output round-trip paired"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" --R2 "$ASSET_DIR/test_2.fastq" -o abcd -t "$SMOKE_THREADS_COMPRESS"
run_spring -d -i abcd -o tmp -t "$SMOKE_THREADS_DECOMPRESS"
sort tmp.1 >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted
sort tmp.2 >tmp.sorted
sort "$ASSET_DIR/test_2.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

announce_case "long-read mode round-trip"
run_spring -c --R1 "$ASSET_DIR/sample.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/sample.fastq"

announce_case "memory capping test"
run_spring -m 0.1 -c --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -m 0.1 -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "archive notes & previewer validation"
run_spring -c --R1 "$ASSET_DIR/test_1.fastq" -n "SMOKE_TEST_NOTE" -o abcd
"${SPRING_PREVIEW_BIN_CMD[@]}" abcd > preview.out
if ! grep -q "SMOKE_TEST_NOTE" preview.out; then
	echo "Failed to find custom note in preview tool output" >&2
	exit 1
fi
if ! grep -q "test_1.fastq" preview.out; then
	echo "Failed to find original filename in preview tool output" >&2
	exit 1
fi

announce_case "lossy mode: ill_bin"
run_spring -c -q ill_bin --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "lossy mode: qvz"
run_spring -c -q qvz 1 --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "lossy mode: binary"
run_spring -c -q binary 30 40 10 --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "stripping: ids"
run_spring -c -s i --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "stripping: quality"
run_spring -c -s q --R1 "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

echo "Tests successful!"
