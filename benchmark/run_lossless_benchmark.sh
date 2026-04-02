#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
SPRING_BIN=${SPRING_BIN:-"$ROOT_DIR/build/spring"}
INPUT_FASTQ=${1:-"$SCRIPT_DIR/sample.fastq"}
THREADS=${THREADS:-8}
BUILD_DIR="$ROOT_DIR/build"
BUILD_LOG="$SCRIPT_DIR/build.log"

ensure_spring_binary() {
  if [[ -x "$SPRING_BIN" ]]; then
    return
  fi

  echo "Spring binary not found; configuring and building quietly..."

  if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" >"$BUILD_LOG" 2>&1; then
    echo "Spring configure failed. Full log: $BUILD_LOG" >&2
    tail -n 40 "$BUILD_LOG" >&2 || true
    exit 1
  fi

  if ! cmake --build "$BUILD_DIR" --target spring -j >>"$BUILD_LOG" 2>&1; then
    echo "Spring build failed. Full log: $BUILD_LOG" >&2
    tail -n 40 "$BUILD_LOG" >&2 || true
    exit 1
  fi

  echo "Spring build completed. Full log: $BUILD_LOG"
}

ensure_spring_binary

if [[ ! -f "$INPUT_FASTQ" ]]; then
  echo "Input FASTQ not found: $INPUT_FASTQ" >&2
  exit 1
fi

INPUT_ABS=$(realpath -m -- "$INPUT_FASTQ")
INPUT_BASENAME=$(basename -- "$INPUT_ABS")
INPUT_STEM=${INPUT_BASENAME%.*}
OUTPUT_DIR="$SCRIPT_DIR/output"
WORK_DIR="$SCRIPT_DIR/scratch/$INPUT_STEM"
OUTPUT_FILE="$OUTPUT_DIR/$INPUT_STEM.spring"
MAX_SHORT_READ_LENGTH=511

MAX_READ_LENGTH=$(awk 'NR % 4 == 2 { if (length($0) > max_len) max_len = length($0) } END { print max_len + 0 }' "$INPUT_ABS")

LONG_MODE_ARGS=()
if (( MAX_READ_LENGTH > MAX_SHORT_READ_LENGTH )); then
  LONG_MODE_ARGS=(-l)
fi

mkdir -p "$OUTPUT_DIR"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
rm -f "$OUTPUT_FILE"

echo "Running Spring lossless compression"
echo "  input:   $INPUT_ABS"
echo "  output:  $OUTPUT_FILE"
echo "  workdir: $WORK_DIR"
echo "  threads: $THREADS"
echo "  max read length: $MAX_READ_LENGTH"
if (( ${#LONG_MODE_ARGS[@]} > 0 )); then
  echo "  mode:    lossless long-read mode (-l)"
else
  echo "  mode:    lossless short-read mode"
fi

"$SPRING_BIN" \
  -c \
  -i "$INPUT_ABS" \
  -o "$OUTPUT_FILE" \
  -w "$WORK_DIR" \
  -t "$THREADS" \
  "${LONG_MODE_ARGS[@]}" \
  -q lossless

INPUT_SIZE=$(stat -c%s "$INPUT_ABS")
OUTPUT_SIZE=$(stat -c%s "$OUTPUT_FILE")

awk -v input_size="$INPUT_SIZE" -v output_size="$OUTPUT_SIZE" '
BEGIN {
  reduction_percent = 0
  compression_ratio = 0
  if (input_size > 0) {
    reduction_percent = (input_size - output_size) * 100 / input_size
    compression_ratio = input_size / output_size
  }
  printf("\nBenchmark result\n")
  printf("  original bytes:   %d\n", input_size)
  printf("  compressed bytes: %d\n", output_size)
  printf("  size reduction:   %.2f%%\n", reduction_percent)
  printf("  compression ratio %.3fx\n", compression_ratio)
}
'