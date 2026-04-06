#!/bin/bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
ASSET_DIR="$SCRIPT_DIR/assets"
BUILD_DIR="$ROOT_DIR/build"
SPRING_BIN="$BUILD_DIR/spring2"
SPRING_BIN_CMD=()
SPRING_TEST_ARGS_CMD=()
SPRING_SMOKE_MODE="${SPRING_SMOKE_MODE:-full}"
SPRING_COMMAND_TIMEOUT_SECONDS="${SPRING_COMMAND_TIMEOUT_SECONDS:-0}"
WORK_DIR=$(mktemp -d "$BUILD_DIR/smoke-test.XXXXXX")
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

if [[ -n "${SPRING_BIN_WRAPPER:-}" ]]; then
	# Split a simple wrapper command such as valgrind into argv form.
	# shellcheck disable=SC2206
	SPRING_BIN_CMD=(${SPRING_BIN_WRAPPER})
	SPRING_BIN_CMD+=("$SPRING_BIN")
else
	SPRING_BIN_CMD=("$SPRING_BIN")
fi

if [[ -n "${SPRING_TEST_ARGS:-}" ]]; then
	# Split simple extra Spring CLI arguments from the environment.
	# shellcheck disable=SC2206
	SPRING_TEST_ARGS_CMD=(${SPRING_TEST_ARGS})
fi

run_spring() {
	local cmd=("${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" "$@")
	printf 'Running Spring command:'
	printf ' %q' "${cmd[@]}"
	printf '\n'
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
	prepare_local_input "$ASSET_DIR/test_1.fastq" win-single-input.fastq
	run_spring -c -i win-single-input.fastq -o win-single
	run_spring -d -i win-single -o win-single-out
	compare_files win-single-out "$ASSET_DIR/test_1.fastq"

	announce_case "paired fastq long-mode round-trip"
	prepare_local_input "$ASSET_DIR/test_1.fastq" win-paired-input-1.fastq
	prepare_local_input "$ASSET_DIR/test_2.fastq" win-paired-input-2.fastq
	run_spring -c -i win-paired-input-1.fastq win-paired-input-2.fastq -o win-paired
	run_spring -d -i win-paired -o win-paired-out
	compare_files win-paired-out.1 "$ASSET_DIR/test_1.fastq"
	compare_files win-paired-out.2 "$ASSET_DIR/test_2.fastq"

	announce_case "gzipped fastq long-mode round-trip"
	prepare_local_input "$ASSET_DIR/test_1.fastq.gz" win-gzip-input.fastq.gz
	run_spring -c -i win-gzip-input.fastq.gz -o win-gzip
	run_spring -d -i win-gzip -o win-gzip-out
	compare_files win-gzip-out "$ASSET_DIR/test_1.fastq"

	echo "Tests successful!"
	exit 0
fi

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" "$ASSET_DIR/test_2.fasta" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fasta"
compare_files tmp.2 "$ASSET_DIR/test_2.fasta"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_files tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fasta"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta.gz" "$ASSET_DIR/test_2.fasta.gz" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fasta"
compare_files tmp.2 "$ASSET_DIR/test_2.fasta"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp.gz
gunzip -f tmp.gz
compare_files tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp.1.gz tmp.2.gz
gunzip -f tmp.1.gz
gunzip -f tmp.2.gz
compare_files tmp.1 "$ASSET_DIR/test_1.fastq"
compare_files tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp -t 5
compare_files tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp -t 5
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd -s o
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp
sort tmp >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" "${SPRING_TEST_ARGS_CMD[@]}" -d -i abcd -o tmp -t 5
sort tmp.1 >tmp.sorted
sort "$ASSET_DIR/test_1.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted
sort tmp.2 >tmp.sorted
sort "$ASSET_DIR/test_2.fastq" >tmp_1.sorted
compare_files tmp.sorted tmp_1.sorted

echo "Tests successful!"
