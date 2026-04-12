#!/bin/bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
ASSET_DIR="$ROOT_DIR/assets/sample-data"
BUILD_DIR="$ROOT_DIR/build"
SPRING_BIN="$BUILD_DIR/spring2"
SPRING_PREVIEW_BIN="$BUILD_DIR/spring2-preview"
SPRING_BIN_CMD=()
SPRING_PREVIEW_BIN_CMD=()
SPRING_TEST_ARGS_CMD=()
SPRING_SMOKE_MODE="${SPRING_SMOKE_MODE:-full}"
SPRING_COMMAND_TIMEOUT_SECONDS="${SPRING_COMMAND_TIMEOUT_SECONDS:-0}"
mkdir -p "$ROOT_DIR/tests/output"
WORK_DIR=$(mktemp -d "$ROOT_DIR/tests/output/smoke-test.XXXXXX")
CURRENT_SMOKE_CASE=""

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
	exit "$exit_code"
}

cleanup() {
	rm -rf "$WORK_DIR"
}

trap cleanup EXIT
trap on_error ERR

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
	if [[ "$SPRING_COMMAND_TIMEOUT_SECONDS" -gt 0 ]] && command -v timeout >/dev/null 2>&1; then
		timeout "$SPRING_COMMAND_TIMEOUT_SECONDS" "${cmd[@]}"
	else
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

prepare_local_input() {
	local source_path="$1"
	local target_name="$2"
	cp "$source_path" "$target_name"
}

cd "$WORK_DIR"

if [[ "$SPRING_SMOKE_MODE" == "quick" ]]; then
	announce_case "single fastq round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fastq" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp "$ASSET_DIR/test_1.fastq"

	announce_case "single fasta round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fasta" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp "$ASSET_DIR/test_1.fasta"

	announce_case "paired fastq round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
	compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

	announce_case "gzipped fastq input round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fastq.gz" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp "$ASSET_DIR/test_1.fastq"

	echo "Tests successful!"
	exit 0
fi

if [[ "$SPRING_SMOKE_MODE" == "windows-quick" ]]; then
	announce_case "single fastq long-mode round-trip"
	run_spring -c -i "$ASSET_DIR/sample.fastq" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp "$ASSET_DIR/sample.fastq"

	announce_case "paired fastq long-mode round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
	compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

	announce_case "gzipped fastq long-mode round-trip"
	run_spring -c -i "$ASSET_DIR/test_1.fastq.gz" -o abcd
	run_spring -d -i abcd -o tmp
	compare_files tmp "$ASSET_DIR/test_1.fastq"

	echo "Tests successful!"
	exit 0
fi

announce_case "fastq round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "fasta round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

announce_case "paired fastq round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fasta round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fasta" "$ASSET_DIR/test_2.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fasta"
compare_files tmp.2 "$ASSET_DIR/test_2.fasta"

announce_case "fastq to gzipped output"
run_spring -c -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"
run_spring -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "fasta round-trip repeat"
run_spring -c -i "$ASSET_DIR/test_1.fasta" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

announce_case "paired fastq round-trip repeat"
run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fastq gzipped input round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fasta gzipped input round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fasta.gz" "$ASSET_DIR/test_2.fasta.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_files tmp.1 "$ASSET_DIR/test_1.fasta"
compare_files tmp.2 "$ASSET_DIR/test_2.fasta"

announce_case "single gzipped fastq round-trip"
run_spring -c -i "$ASSET_DIR/test_1.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "single gzipped fastq to gzipped output"
run_spring -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "paired fastq gzipped input round-trip redundant"
run_spring -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
run_spring -d -i abcd -o tmp -u
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "paired fastq gzipped input to gzipped outputs"
run_spring -d -i abcd -o tmp.1.gz tmp.2.gz
gunzip -f tmp.1.gz
gunzip -f tmp.2.gz
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "multi-threaded round-trip single"
run_spring -c -i "$ASSET_DIR/test_1.fastq" -o abcd -t 8
run_spring -d -i abcd -o tmp -t 5
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "multi-threaded round-trip paired"
run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
run_spring -d -i abcd -o tmp -t 5
if ! cmp tmp.1 "$ASSET_DIR/test_1.fastq"; then
	echo "MISMATCH FOUND"
	head -c 20 tmp.1 | cat -A
	echo "EXPECTED:"
	head -c 20 "$ASSET_DIR/test_1.fastq" | cat -A
fi
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

announce_case "sorted output round-trip single"
run_spring -c -i "$ASSET_DIR/test_1.fastq" -o abcd -s o
run_spring -d -i abcd -o tmp
sort tmp >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

announce_case "sorted output round-trip paired"
run_spring -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
run_spring -d -i abcd -o tmp -t 5
sort tmp.1 >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted
sort tmp.2 >tmp.sorted
sort "$ASSET_DIR/test_2.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

announce_case "long-read mode round-trip"
run_spring -c -i "$ASSET_DIR/sample.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/sample.fastq"

announce_case "memory capping test"
run_spring -m 0.1 -c -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -m 0.1 -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

announce_case "archive notes & previewer validation"
run_spring -c -i "$ASSET_DIR/test_1.fastq" -n "SMOKE_TEST_NOTE" -o abcd
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
run_spring -c -q ill_bin -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "lossy mode: qvz"
run_spring -c -q qvz 1 -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "lossy mode: binary"
run_spring -c -q binary 30 40 10 -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "stripping: ids"
run_spring -c -s i -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

announce_case "stripping: quality"
run_spring -c -s q -i "$ASSET_DIR/test_1.fastq" -o abcd
run_spring -d -i abcd -o tmp
compare_lines tmp "$ASSET_DIR/test_1.fastq"

echo "Tests successful!"
