#!/bin/bash

set -e

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
ASSET_DIR="$SCRIPT_DIR/assets"
BUILD_DIR="$ROOT_DIR/build"
SPRING_BIN="$BUILD_DIR/spring"
SPRING_BIN_CMD=()
WORK_DIR=$(mktemp -d "$BUILD_DIR/smoke-test.XXXXXX")

cleanup() {
	rm -rf "$WORK_DIR"
}

trap cleanup EXIT

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

cd "$WORK_DIR"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp "$ASSET_DIR/test_1.fasta"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" "$ASSET_DIR/test_2.fasta" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fasta"
cmp tmp.2 "$ASSET_DIR/test_2.fasta"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd -l
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp "$ASSET_DIR/test_1.fastq"
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp.gz
gunzip -f tmp.gz
cmp tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta" -o abcd -l
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp "$ASSET_DIR/test_1.fasta"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -l
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fasta.gz" "$ASSET_DIR/test_2.fasta.gz" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fasta"
cmp tmp.2 "$ASSET_DIR/test_2.fasta"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp.gz
gunzip -f tmp.gz
cmp tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq.gz" "$ASSET_DIR/test_2.fastq.gz" -o abcd
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp.1.gz tmp.2.gz
gunzip -f tmp.1.gz
gunzip -f tmp.2.gz
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp -t 5
cmp tmp "$ASSET_DIR/test_1.fastq"

"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp -t 5
cmp tmp.1 "$ASSET_DIR/test_1.fastq"
cmp tmp.2 "$ASSET_DIR/test_2.fastq"


"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" -o abcd -r
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp
sort tmp > tmp.sorted
sort "$ASSET_DIR/test_1.fastq" > tmp_1.sorted
cmp tmp.sorted tmp_1.sorted


"${SPRING_BIN_CMD[@]}" -c -i "$ASSET_DIR/test_1.fastq" "$ASSET_DIR/test_2.fastq" -o abcd -t 8
"${SPRING_BIN_CMD[@]}" -d -i abcd -o tmp -t 5
sort tmp.1 > tmp.sorted
sort "$ASSET_DIR/test_1.fastq" > tmp_1.sorted
cmp tmp.sorted tmp_1.sorted
sort tmp.2 > tmp.sorted
sort "$ASSET_DIR/test_2.fastq" > tmp_1.sorted
cmp tmp.sorted tmp_1.sorted

echo "Tests successful!"
