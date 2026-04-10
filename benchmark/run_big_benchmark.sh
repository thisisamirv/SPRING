#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
SPRING_BIN=${SPRING_BIN:-"$ROOT_DIR/build/spring2"}
THREADS=${THREADS:-8}
BUILD_DIR="$ROOT_DIR/build"
TMP_DIR="$SCRIPT_DIR/tmp"
TMP_INPUT_DIR="$TMP_DIR/input"
TMP_LOG_DIR="$TMP_DIR/logs"
TMP_OUTPUT_DIR="$TMP_DIR/output"
TMP_WORK_DIR="$TMP_DIR/work"
DOWNLOAD_URL="https://figshare.com/ndownloader/files/38965664"
DEFAULT_INPUT_FASTQ="$TMP_INPUT_DIR/04-CC002-659-M_S4_L001_R2_001.fastq.gz"
INPUT_FASTQ=${INPUT_FASTQ:-"$DEFAULT_INPUT_FASTQ"}
BUILD_LOG="$TMP_LOG_DIR/build.log"
COMPRESS_RESOURCE_LOG="$TMP_LOG_DIR/compress_resource_usage.log"
DECOMPRESS_RESOURCE_LOG="$TMP_LOG_DIR/decompress_resource_usage.log"

remove_empty_dir_if_present() {
	local dir_path="$1"

	if [[ -d "$dir_path" ]] && [[ -z "$(find "$dir_path" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
		rmdir "$dir_path"
	fi
}

trap 'remove_empty_dir_if_present "$TMP_WORK_DIR"' EXIT

TIME_BIN=""
if [[ -x /usr/bin/time ]]; then
	TIME_BIN=/usr/bin/time
elif [[ -n "$(type -P time)" ]]; then
	TIME_BIN=$(type -P time)
fi

compute_checksum() {
	local file_path="$1"

	if [[ -n "$(type -P sha256sum)" ]]; then
		sha256sum "$file_path" | awk '{print $1}'
		return
	fi

	if [[ -n "$(type -P shasum)" ]]; then
		shasum -a 256 "$file_path" | awk '{print $1}'
		return
	fi
}

stream_input_bytes() {
	local file_path="$1"

	if [[ "$file_path" == *.gz ]]; then
		gzip -dc -- "$file_path"
		return
	fi

	cat -- "$file_path"
}

compute_normalized_input_checksum() {
	local file_path="$1"

	if [[ -n "$(type -P sha256sum)" ]]; then
		stream_input_bytes "$file_path" | sha256sum | awk '{print $1}'
		return
	fi

	if [[ -n "$(type -P shasum)" ]]; then
		stream_input_bytes "$file_path" | shasum -a 256 | awk '{print $1}'
		return
	fi
}

ensure_benchmark_input() {
	mkdir -p "$TMP_INPUT_DIR" "$TMP_LOG_DIR" "$TMP_OUTPUT_DIR" "$TMP_WORK_DIR"

	if [[ "$INPUT_FASTQ" != "$DEFAULT_INPUT_FASTQ" ]]; then
		if [[ ! -f "$INPUT_FASTQ" ]]; then
			echo "Requested INPUT_FASTQ does not exist: $INPUT_FASTQ" >&2
			exit 1
		fi
		return
	fi

	if [[ -f "$DEFAULT_INPUT_FASTQ" ]]; then
		return
	fi

	cat >&2 <<EOF
Benchmark input not found:
  $DEFAULT_INPUT_FASTQ

Create benchmark/tmp/input/ and download this file from:
  $DOWNLOAD_URL

Save it as:
  $DEFAULT_INPUT_FASTQ

Or run the script with a local override:
  INPUT_FASTQ=/path/to/04-CC002-659-M_S4_L001_R2_001.fastq.gz ./benchmark/run_big_benchmark.sh
EOF
	exit 1
}

ensure_spring_binary() {
	if [[ -x "$SPRING_BIN" ]]; then
		return
	fi

	mkdir -p "$TMP_LOG_DIR"
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

run_with_resource_log() {
	local log_file="$1"
	shift

	rm -f "$log_file"

	if [[ -n "$TIME_BIN" ]]; then
		"$TIME_BIN" \
			-f 'elapsed_seconds=%e
user_seconds=%U
system_seconds=%S
cpu_percent=%P
max_rss_kb=%M' \
			-o "$log_file" \
			"$@"
		return
	fi

	TIMEFORMAT=$'elapsed_seconds=%3R\nuser_seconds=%3U\nsystem_seconds=%3S\ncpu_percent=unavailable\nmax_rss_kb=unavailable'
	exec 3>&2
	{ time "$@" 2>&3; } 2>"$log_file"
	exec 3>&-
}

populate_resource_vars() {
	local prefix="$1"
	local log_file="$2"
	local key
	local value

	printf -v "${prefix}_elapsed_seconds" '%s' ""
	printf -v "${prefix}_user_seconds" '%s' ""
	printf -v "${prefix}_system_seconds" '%s' ""
	printf -v "${prefix}_cpu_percent" '%s' ""
	printf -v "${prefix}_max_rss_kb" '%s' ""

	if [[ ! -f "$log_file" ]]; then
		return
	fi

	while IFS='=' read -r key value; do
		case "$key" in
		elapsed_seconds) printf -v "${prefix}_elapsed_seconds" '%s' "$value" ;;
		user_seconds) printf -v "${prefix}_user_seconds" '%s' "$value" ;;
		system_seconds) printf -v "${prefix}_system_seconds" '%s' "$value" ;;
		cpu_percent) printf -v "${prefix}_cpu_percent" '%s' "$value" ;;
		max_rss_kb) printf -v "${prefix}_max_rss_kb" '%s' "$value" ;;
		esac
	done <"$log_file"
}

ensure_benchmark_input
ensure_spring_binary

INPUT_ABS=$(realpath -m -- "$INPUT_FASTQ")
INPUT_BASENAME=$(basename -- "$INPUT_ABS")
INPUT_STEM=${INPUT_BASENAME%.*}
OUTPUT_DIR="$TMP_OUTPUT_DIR"
WORK_DIR="$TMP_WORK_DIR/$INPUT_STEM.work"
OUTPUT_FILE="$OUTPUT_DIR/$INPUT_STEM.sp"
DECOMPRESSED_OUTPUT_FILE="$OUTPUT_DIR/$INPUT_STEM.roundtrip.fastq"
MAX_SHORT_READ_LENGTH=511

MAX_READ_LENGTH=$(stream_input_bytes "$INPUT_ABS" | awk 'NR % 4 == 2 { if (length($0) > max_len) max_len = length($0) } END { print max_len + 0 }')


mkdir -p "$TMP_INPUT_DIR" "$TMP_LOG_DIR" "$OUTPUT_DIR" "$TMP_WORK_DIR"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
rm -f "$OUTPUT_FILE"
rm -f "$DECOMPRESSED_OUTPUT_FILE"
rm -f "$COMPRESS_RESOURCE_LOG"
rm -f "$DECOMPRESS_RESOURCE_LOG"

echo "Running Spring lossless compression"
echo "  input:   $INPUT_ABS"
echo "  output:  $OUTPUT_FILE"
echo "  workdir: $WORK_DIR"
echo "  threads: $THREADS"
echo "  max read length: $MAX_READ_LENGTH"
echo "  mode:    lossless"

spring_args=(
	-c
	-i "$INPUT_ABS"
	-o "$OUTPUT_FILE"
	-w "$WORK_DIR"
	-t "$THREADS"
	-q lossless
	-n "Big Benchmark"
)

run_with_resource_log "$COMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${spring_args[@]}"

echo "Running Spring decompression"
echo "  input:   $OUTPUT_FILE"
echo "  output:  $DECOMPRESSED_OUTPUT_FILE"

decompress_args=(
	-d
	-i "$OUTPUT_FILE"
	-o "$DECOMPRESSED_OUTPUT_FILE"
)

run_with_resource_log "$DECOMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${decompress_args[@]}"

remove_empty_dir_if_present "$WORK_DIR"

INPUT_SIZE=$(stat -c%s "$INPUT_ABS")
OUTPUT_SIZE=$(stat -c%s "$OUTPUT_FILE")
DECOMPRESSED_SIZE=$(stat -c%s "$DECOMPRESSED_OUTPUT_FILE")

populate_resource_vars "compress" "$COMPRESS_RESOURCE_LOG"
populate_resource_vars "decompress" "$DECOMPRESS_RESOURCE_LOG"

integrity_method="byte comparison only"
original_checksum=""
decompressed_checksum=""
checksum_status="unavailable"

if [[ -n "$(type -P sha256sum)" || -n "$(type -P shasum)" ]]; then
	integrity_method="SHA-256 + byte comparison"
	original_checksum=$(compute_normalized_input_checksum "$INPUT_ABS")
	decompressed_checksum=$(compute_checksum "$DECOMPRESSED_OUTPUT_FILE")
	if [[ "$original_checksum" == "$decompressed_checksum" ]]; then
		checksum_status="match"
	else
		checksum_status="mismatch"
	fi
fi

roundtrip_status="different"
if cmp -s <(stream_input_bytes "$INPUT_ABS") "$DECOMPRESSED_OUTPUT_FILE"; then
	roundtrip_status="identical"
fi

awk \
	-v input_size="$INPUT_SIZE" \
	-v output_size="$OUTPUT_SIZE" \
	-v decompressed_size="$DECOMPRESSED_SIZE" \
	-v compress_elapsed_seconds="$compress_elapsed_seconds" \
	-v compress_user_seconds="$compress_user_seconds" \
	-v compress_system_seconds="$compress_system_seconds" \
	-v compress_cpu_percent="$compress_cpu_percent" \
	-v compress_max_rss_kb="$compress_max_rss_kb" \
	-v decompress_elapsed_seconds="$decompress_elapsed_seconds" \
	-v decompress_user_seconds="$decompress_user_seconds" \
	-v decompress_system_seconds="$decompress_system_seconds" \
	-v decompress_cpu_percent="$decompress_cpu_percent" \
	-v decompress_max_rss_kb="$decompress_max_rss_kb" \
	-v integrity_method="$integrity_method" \
	-v original_checksum="$original_checksum" \
	-v decompressed_checksum="$decompressed_checksum" \
	-v checksum_status="$checksum_status" \
	-v roundtrip_status="$roundtrip_status" '
BEGIN {
  reduction_percent = 0
  compression_ratio = 0
  compress_max_rss_mb = 0
  compress_average_core_usage = 0
  decompress_max_rss_mb = 0
  decompress_average_core_usage = 0
  if (input_size > 0) {
    reduction_percent = (input_size - output_size) * 100 / input_size
    compression_ratio = input_size / output_size
  }
  if (compress_elapsed_seconds != "" && compress_elapsed_seconds != 0 && compress_user_seconds != "" && compress_system_seconds != "") {
    compress_average_core_usage = (compress_user_seconds + compress_system_seconds) / compress_elapsed_seconds
  }
  if (decompress_elapsed_seconds != "" && decompress_elapsed_seconds != 0 && decompress_user_seconds != "" && decompress_system_seconds != "") {
    decompress_average_core_usage = (decompress_user_seconds + decompress_system_seconds) / decompress_elapsed_seconds
  }
  if (compress_max_rss_kb != "" && compress_max_rss_kb != "unavailable") {
    compress_max_rss_mb = compress_max_rss_kb / 1024
  }
  if (decompress_max_rss_kb != "" && decompress_max_rss_kb != "unavailable") {
    decompress_max_rss_mb = decompress_max_rss_kb / 1024
  }
  printf("\nBenchmark result\n")
  printf("  original bytes:   %d\n", input_size)
  printf("  compressed bytes: %d\n", output_size)
  printf("  decompressed bytes: %d\n", decompressed_size)
  printf("  size reduction:   %.2f%%\n", reduction_percent)
  printf("  compression ratio %.3fx\n", compression_ratio)

  printf("\nCompression resources\n")
  if (compress_elapsed_seconds != "") {
    printf("  elapsed time:     %ss\n", compress_elapsed_seconds)
  }
  if (compress_cpu_percent != "") {
    printf("  cpu usage:        %s\n", compress_cpu_percent)
  }
  if (compress_user_seconds != "" || compress_system_seconds != "") {
    printf("  cpu time:         user %ss, system %ss\n", compress_user_seconds, compress_system_seconds)
  }
  if (compress_average_core_usage > 0) {
    printf("  avg core usage:   %.2f cores\n", compress_average_core_usage)
  }
  if (compress_max_rss_kb != "" && compress_max_rss_kb != "unavailable") {
    printf("  peak memory:      %d KB (%.2f MB RSS)\n", compress_max_rss_kb, compress_max_rss_mb)
  } else if (compress_elapsed_seconds != "") {
    printf("  peak memory:      unavailable (install GNU time for RSS reporting)\n")
  }

  printf("\nDecompression resources\n")
  if (decompress_elapsed_seconds != "") {
    printf("  elapsed time:     %ss\n", decompress_elapsed_seconds)
  }
  if (decompress_cpu_percent != "") {
    printf("  cpu usage:        %s\n", decompress_cpu_percent)
  }
  if (decompress_user_seconds != "" || decompress_system_seconds != "") {
    printf("  cpu time:         user %ss, system %ss\n", decompress_user_seconds, decompress_system_seconds)
  }
  if (decompress_average_core_usage > 0) {
    printf("  avg core usage:   %.2f cores\n", decompress_average_core_usage)
  }
  if (decompress_max_rss_kb != "" && decompress_max_rss_kb != "unavailable") {
    printf("  peak memory:      %d KB (%.2f MB RSS)\n", decompress_max_rss_kb, decompress_max_rss_mb)
  } else if (decompress_elapsed_seconds != "") {
    printf("  peak memory:      unavailable (install GNU time for RSS reporting)\n")
  }

  printf("\nRound-trip check\n")
  printf("  integrity method: %s\n", integrity_method)
  if (original_checksum != "") {
    printf("  original checksum: %s\n", original_checksum)
  }
  if (decompressed_checksum != "") {
    printf("  output checksum:   %s\n", decompressed_checksum)
  }
  if (checksum_status != "unavailable") {
    printf("  checksum status:  %s\n", checksum_status)
  }
  printf("  decompressed file matches input: %s\n", roundtrip_status)
}
'
