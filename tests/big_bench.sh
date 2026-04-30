#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
SPRING_BIN=${SPRING_BIN:-"$ROOT_DIR/build/spring2"}
THREADS=${THREADS:-8}
BUILD_DIR="$ROOT_DIR/build"
TMP_DIR="$SCRIPT_DIR/output"
TMP_INPUT_DIR="$TMP_DIR/input"
TMP_LOG_DIR="$TMP_DIR/logs"
TMP_OUTPUT_DIR="$TMP_DIR/runs"
TMP_WORK_DIR="$TMP_DIR/work"
BIG_BENCH_LOG="$TMP_LOG_DIR/big_bench.log"
NO_DEBUG=0

while (($#)); do
	case "$1" in
	--no_debug)
		NO_DEBUG=1
		;;
	*)
		echo "Unknown argument: $1" >&2
		exit 1
		;;
	esac
	shift
done

SPRING_VERBOSE_ARGS=()
if [[ "$NO_DEBUG" -eq 0 ]]; then
	SPRING_VERBOSE_ARGS=(-v debug)
fi

# Dataset: SRR2990433 (EBI FTP)
URL_R1="ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_1.fastq.gz"
URL_R2="ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_2.fastq.gz"

PATH_R1="$TMP_INPUT_DIR/SRR8185389_1.fastq.gz"
PATH_R2="$TMP_INPUT_DIR/SRR8185389_2.fastq.gz"

BUILD_LOG="$TMP_LOG_DIR/build.log"
COMPRESS_RESOURCE_LOG="$TMP_LOG_DIR/compress_resource_usage.log"
DECOMPRESS_RESOURCE_LOG="$TMP_LOG_DIR/decompress_resource_usage.log"

mkdir -p "$TMP_LOG_DIR"
: >"$BIG_BENCH_LOG"
exec > >(tee -a "$BIG_BENCH_LOG") 2>&1

remove_empty_dir_if_present() {
	local dir_path="$1"
	if [[ -d "$dir_path" ]] && [[ -z "$(find "$dir_path" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
		rmdir "$dir_path"
	fi
}

trap 'remove_empty_dir_if_present "$TMP_WORK_DIR"' EXIT

download_file() {
	local url="$1"
	local dest="$2"
	if [[ -f "$dest" ]]; then return; fi
	echo "Downloading $(basename "$dest")..."
	if type curl >/dev/null 2>&1; then
		curl -L -# -o "$dest" "$url"
	elif type wget >/dev/null 2>&1; then
		wget --progress=bar:force -O "$dest" "$url"
	else
		echo "Error: Neither curl nor wget found." >&2
		exit 1
	fi
}

ensure_benchmark_input() {
	mkdir -p "$TMP_INPUT_DIR" "$TMP_LOG_DIR" "$TMP_OUTPUT_DIR" "$TMP_WORK_DIR"
	download_file "$URL_R1" "$PATH_R1"
	download_file "$URL_R2" "$PATH_R2"
}

ensure_spring_binary() {
	if [[ -x "$SPRING_BIN" ]]; then return; fi
	mkdir -p "$TMP_LOG_DIR"
	echo "Spring binary not found; building..."
	cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DSPRING_STATIC_RUNTIMES=OFF >"$BUILD_LOG" 2>&1
	cmake --build "$BUILD_DIR" --target spring -j >>"$BUILD_LOG" 2>&1
}

run_with_resource_log() {
	local log_file="$1"
	shift
	rm -f "$log_file"
	local time_bin=""
	if [[ -x /usr/bin/time ]]; then time_bin=/usr/bin/time; elif [[ -n "$(type -P time)" ]]; then time_bin=$(type -P time); fi
	if [[ -n "$time_bin" ]]; then
		"$time_bin" -f 'elapsed_seconds=%e\nuser_seconds=%U\nsystem_seconds=%S\ncpu_percent=%P\nmax_rss_kb=%M' -o "$log_file" "$@"
		return
	fi
	TIMEFORMAT=$'elapsed_seconds=%3R\nuser_seconds=%3U\nsystem_seconds=%3S\ncpu_percent=unavailable\nmax_rss_kb=unavailable'
	{ time "$@"; } 2>"$log_file"
}

print_step_timings() {
	if [[ ! -f "$BIG_BENCH_LOG" ]]; then
		echo -e "\nStep timings"
		echo "  No step timings found."
		return
	fi

	local summary
	summary=$(awk '
	BEGIN {
	  pending = ""
	  count = 0
	}
	/^[[:space:]]*.+ \.\.\.[[:space:]]*$/ {
	  pending = $0
	  sub(/^[[:space:]]*/, "", pending)
	  sub(/[[:space:]]*\.\.\.[[:space:]]*$/, "", pending)
	  next
	}
	/^[[:space:]]*Time for this step:[[:space:]]*/ {
	  if (pending != "") {
	    line = $0
	    sub(/^[[:space:]]*Time for this step:[[:space:]]*/, "", line)
	    printf("  %02d. %s: %s\n", ++count, pending, line)
	    pending = ""
	  }
	  next
	}
	/^[[:space:]]*Total time for (compression|decompression):[[:space:]]*/ {
	  line = $0
	  sub(/^[[:space:]]*/, "", line)
	  printf("  %02d. %s\n", ++count, line)
	}
	' "$BIG_BENCH_LOG")

	echo -e "\nStep timings"
	if [[ -z "$summary" ]]; then
		echo "  No step timings found."
	else
		printf '%s\n' "$summary"
	fi
}

populate_resource_vars() {
	local prefix="$1"
	local log_file="$2"
	if [[ ! -f "$log_file" ]]; then return; fi
	while IFS='=' read -r key value; do
		case "$key" in
		elapsed_seconds) printf -v "${prefix}_elapsed_seconds" '%s' "$value" ;;
		user_seconds) printf -v "${prefix}_user_seconds" '%s' "$value" ;;
		system_seconds) printf -v "${prefix}_system_seconds" '%s' "$value" ;;
		max_rss_kb) printf -v "${prefix}_max_rss_kb" '%s' "$value" ;;
		esac
	done <"$log_file"
}

ensure_benchmark_input
ensure_spring_binary

INPUT_STEM="SRR8185389_pe"
WORK_DIR="$TMP_WORK_DIR/$INPUT_STEM.work"
OUTPUT_FILE="$TMP_OUTPUT_DIR/$INPUT_STEM.sp"
DECOMP_BASE="$TMP_OUTPUT_DIR/$INPUT_STEM.roundtrip.fastq"
DECOMP_FILE_1="$DECOMP_BASE.1"
DECOMP_FILE_2="$DECOMP_BASE.2"

rm -rf "$WORK_DIR" && mkdir -p "$WORK_DIR"
rm -f "$OUTPUT_FILE" "$DECOMP_FILE_1" "$DECOMP_FILE_2"

echo "Running Spring paired-end compression (SRR2990433)"
run_with_resource_log "$COMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${SPRING_VERBOSE_ARGS[@]}" -c --R1 "$PATH_R1" --R2 "$PATH_R2" -o "$OUTPUT_FILE" -w "$WORK_DIR" -t "$THREADS" -q lossless

echo "Running Spring decompression"
run_with_resource_log "$DECOMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${SPRING_VERBOSE_ARGS[@]}" -d -i "$OUTPUT_FILE" -o "$DECOMP_BASE" -w "$WORK_DIR"

# Results
INPUT_SIZE=$(($(stat -c%s "$PATH_R1") + $(stat -c%s "$PATH_R2")))
OUTPUT_SIZE=$(stat -c%s "$OUTPUT_FILE")
DECOMP_SIZE=$(($(stat -c%s "$DECOMP_FILE_1") + $(stat -c%s "$DECOMP_FILE_2")))

populate_resource_vars "compress" "$COMPRESS_RESOURCE_LOG"
populate_resource_vars "decompress" "$DECOMPRESS_RESOURCE_LOG"

echo -e "\nBenchmark result (SRR2990433 Paired-End)"
echo "  original bytes:   $INPUT_SIZE"
echo "  compressed bytes: $OUTPUT_SIZE"
echo "  decompressed bytes: $DECOMP_SIZE"
echo "  compression pass time:    ${compress_elapsed_seconds:-unavailable}s"
echo "  decompression pass time:  ${decompress_elapsed_seconds:-unavailable}s"

check_hash() {
	local orig="$1"
	local decomp="$2"
	local h1=$(gzip -dc "$orig" | sha256sum | awk '{print $1}')
	local h2=$(sha256sum "$decomp" | awk '{print $1}')
	if [[ "$h1" == "$h2" ]]; then echo "MATCH"; else echo "MISMATCH"; fi
}

echo -e "\nVerifying integrity..."
echo "  Read 1: $(check_hash "$PATH_R1" "$DECOMP_FILE_1")"
echo "  Read 2: $(check_hash "$PATH_R2" "$DECOMP_FILE_2")"

print_step_timings
