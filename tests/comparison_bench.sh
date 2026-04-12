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
URL_R1="ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_1.fastq.gz"
URL_R2="ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_2.fastq.gz"
DEFAULT_PATH_R1="$TMP_INPUT_DIR/SRR8185389_1.fastq.gz"
DEFAULT_PATH_R2="$TMP_INPUT_DIR/SRR8185389_2.fastq.gz"
INPUT_FASTQ_1=${INPUT_FASTQ_1:-"$DEFAULT_PATH_R1"}
INPUT_FASTQ_2=${INPUT_FASTQ_2:-"$DEFAULT_PATH_R2"}
BUILD_LOG="$TMP_LOG_DIR/build.log"
SPRING_V1_ENV_NAME="spring_v1"

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

MAMBA_BIN=""
SPRING_V1_RUNNER=""

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

	if [[ "$INPUT_FASTQ_1" == "$DEFAULT_PATH_R1" ]]; then
		download_file "$URL_R1" "$DEFAULT_PATH_R1"
	fi
	if [[ "$INPUT_FASTQ_2" == "$DEFAULT_PATH_R2" ]]; then
		download_file "$URL_R2" "$DEFAULT_PATH_R2"
	fi

	if [[ ! -f "$INPUT_FASTQ_1" ]]; then
		echo "Primary INPUT_FASTQ_1 does not exist: $INPUT_FASTQ_1" >&2
		exit 1
	fi
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

ensure_spring_v1_runner() {
	MAMBA_BIN=$(type -P mamba || true)
	if [[ -z "$MAMBA_BIN" ]]; then
		cat >&2 <<EOF
This comparison benchmark requires mamba, but it is not installed or not on PATH.

Install mamba, create the $SPRING_V1_ENV_NAME environment, and install the original Spring v1 in that environment.
EOF
		exit 1
	fi

	if ! "$MAMBA_BIN" env list | awk 'NR > 2 {print $1}' | grep -Fxq "$SPRING_V1_ENV_NAME"; then
		cat >&2 <<EOF
Required mamba environment not found: $SPRING_V1_ENV_NAME

Create the $SPRING_V1_ENV_NAME environment and install the original Spring v1 in it before running this benchmark.
EOF
		exit 1
	fi

	if ! "$MAMBA_BIN" run -n "$SPRING_V1_ENV_NAME" spring --help >/dev/null 2>&1; then
		cat >&2 <<EOF
The $SPRING_V1_ENV_NAME environment exists, but the 'spring' executable is not available in it.

Install the original Spring v1 package/binary into that environment before running this benchmark.
EOF
		exit 1
	fi

	mkdir -p "$TMP_LOG_DIR"
	SPRING_V1_RUNNER="$TMP_LOG_DIR/spring_v1_runner.sh"
	cat >"$SPRING_V1_RUNNER" <<EOF
#!/usr/bin/env bash
set -euo pipefail
eval "\$($MAMBA_BIN shell hook --shell bash)"
mamba activate $SPRING_V1_ENV_NAME
exec spring "\$@"
EOF
	chmod +x "$SPRING_V1_RUNNER"
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

spring_supports_gzip_flag() {
	local runner="$1"
	"$runner" --help 2>&1 | grep -q 'gzipped-fastq'
}

write_metrics_file() {
	local metrics_path="$1"
	shift

	: >"$metrics_path"
	while (($#)); do
		printf '%s\n' "$1" >>"$metrics_path"
		shift
	done
}

load_metrics_file() {
	local prefix="$1"
	local metrics_path="$2"
	local key
	local value

	while IFS='=' read -r key value; do
		printf -v "${prefix}_${key}" '%s' "$value"
	done <"$metrics_path"
}

print_benchmark_summary() {
	local label="$1"
	local input_size="$2"
	local output_size="$3"
	local decompressed_size="$4"
	local compress_elapsed="$5"
	local compress_user="$6"
	local compress_system="$7"
	local compress_cpu="$8"
	local compress_rss="$9"
	local decompress_elapsed="${10}"
	local decompress_user="${11}"
	local decompress_system="${12}"
	local decompress_cpu="${13}"
	local decompress_rss="${14}"
	local integrity_method="${15}"
	local original_checksum="${16}"
	local decompressed_checksum="${17}"
	local checksum_status="${18}"
	local roundtrip_status="${19}"

	awk \
		-v label="$label" \
		-v input_size="$input_size" \
		-v output_size="$output_size" \
		-v decompressed_size="$decompressed_size" \
		-v compress_elapsed_seconds="$compress_elapsed" \
		-v compress_user_seconds="$compress_user" \
		-v compress_system_seconds="$compress_system" \
		-v compress_cpu_percent="$compress_cpu" \
		-v compress_max_rss_kb="$compress_rss" \
		-v decompress_elapsed_seconds="$decompress_elapsed" \
		-v decompress_user_seconds="$decompress_user" \
		-v decompress_system_seconds="$decompress_system" \
		-v decompress_cpu_percent="$decompress_cpu" \
		-v decompress_max_rss_kb="$decompress_rss" \
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
  printf("\nBenchmark result (%s)\n", label)
  printf("  original bytes:   %d\n", input_size)
  printf("  compressed bytes: %d\n", output_size)
  printf("  decompressed bytes: %d\n", decompressed_size)
  printf("  size reduction:   %.2f%%\n", reduction_percent)
  printf("  compression ratio %.3fx\n", compression_ratio)

  printf("\nCompression resources (%s)\n", label)
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

  printf("\nDecompression resources (%s)\n", label)
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

  printf("\nRound-trip check (%s)\n", label)
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
}

run_benchmark() {
	local label="$1"
	local display_name="$2"
	local runner="$3"

	local output_prefix="$TMP_OUTPUT_DIR/$INPUT_STEM.$label"
	local output_file="$output_prefix.sp"
	local decompressed_output_file="$output_prefix.roundtrip.fastq"
	local work_dir="$TMP_WORK_DIR/$INPUT_STEM.$label.work"
	local compress_resource_log="$TMP_LOG_DIR/${label}_compress_resource_usage.log"
	local decompress_resource_log="$TMP_LOG_DIR/${label}_decompress_resource_usage.log"
	local metrics_path="$TMP_LOG_DIR/${label}.metrics"
	local input_size
	local output_size
	local decompressed_size
	local integrity_method="byte comparison only"
	local original_checksum=""
	local decompressed_checksum=""
	local checksum_status="unavailable"
	local roundtrip_status="different"

	mkdir -p "$TMP_INPUT_DIR" "$TMP_LOG_DIR" "$TMP_OUTPUT_DIR" "$TMP_WORK_DIR"
	rm -rf "$work_dir"
	mkdir -p "$work_dir"
	rm -f "$output_file" "$decompressed_output_file"
	rm -f "$compress_resource_log" "$decompress_resource_log" "$metrics_path"

	echo "Running $display_name lossless compression"
	echo "  input:   $INPUT_ABS"
	echo "  output:  $output_file"
	echo "  workdir: $work_dir"
	echo "  threads: $THREADS"
	echo "  max read length: $MAX_READ_LENGTH"
	if ((${MAX_READ_LENGTH} > ${MAX_SHORT_READ_LENGTH})) && [[ "$label" == "spring_v1" ]]; then
		echo "  mode:    lossless long-read mode (-l)"
	else
		echo "  mode:    lossless"
	fi

	local spring_args=(
		-c
		-i "$INPUT_ABS_1"
	)
	if [[ -n "$INPUT_ABS_2" ]]; then
		spring_args+=("$INPUT_ABS_2")
	fi
	spring_args+=(
		-o "$output_file"
		-w "$work_dir"
		-t "$THREADS"
		-q lossless
	)
	if ((${MAX_READ_LENGTH} > ${MAX_SHORT_READ_LENGTH})) && [[ "$label" == "spring_v1" ]]; then
		spring_args+=(-l)
	fi
	if [[ "$INPUT_ABS_1" == *.gz ]] && spring_supports_gzip_flag "$runner"; then
		spring_args=(-g "${spring_args[@]}")
	fi

	run_with_resource_log "$compress_resource_log" "$runner" "${spring_args[@]}"

	echo "Running $display_name decompression"
	echo "  input:   $output_file"
	echo "  output:  $decompressed_output_file"

	local decompress_args=(
		-d
		-i "$output_file"
		-o "$decompressed_output_file"
	)

	run_with_resource_log "$decompress_resource_log" "$runner" "${decompress_args[@]}"

	input_size=$(stat -c%s "$INPUT_ABS")
	output_size=$(stat -c%s "$output_file")
	decompressed_size=$(stat -c%s "$decompressed_output_file")

	populate_resource_vars "${label}_compress" "$compress_resource_log"
	populate_resource_vars "${label}_decompress" "$decompress_resource_log"

	local compress_elapsed_var="${label}_compress_elapsed_seconds"
	local compress_user_var="${label}_compress_user_seconds"
	local compress_system_var="${label}_compress_system_seconds"
	local compress_cpu_var="${label}_compress_cpu_percent"
	local compress_rss_var="${label}_compress_max_rss_kb"
	local decompress_elapsed_var="${label}_decompress_elapsed_seconds"
	local decompress_user_var="${label}_decompress_user_seconds"
	local decompress_system_var="${label}_decompress_system_seconds"
	local decompress_cpu_var="${label}_decompress_cpu_percent"
	local decompress_rss_var="${label}_decompress_max_rss_kb"

	local compress_elapsed="${!compress_elapsed_var}"
	local compress_user="${!compress_user_var}"
	local compress_system="${!compress_system_var}"
	local compress_cpu="${!compress_cpu_var}"
	local compress_rss="${!compress_rss_var}"
	local decompress_elapsed="${!decompress_elapsed_var}"
	local decompress_user="${!decompress_user_var}"
	local decompress_system="${!decompress_system_var}"
	local decompress_cpu="${!decompress_cpu_var}"
	local decompress_rss="${!decompress_rss_var}"

	if [[ -n "$(type -P sha256sum)" || -n "$(type -P shasum)" ]]; then
		integrity_method="SHA-256 + byte comparison"
		original_checksum=$(compute_normalized_input_checksum "$INPUT_ABS")
		decompressed_checksum=$(compute_checksum "$decompressed_output_file")
		if [[ "$original_checksum" == "$decompressed_checksum" ]]; then
			checksum_status="match"
		else
			checksum_status="mismatch"
		fi
	fi

	if cmp -s <(stream_input_bytes "$INPUT_ABS") "$decompressed_output_file"; then
		roundtrip_status="identical"
	fi

	print_benchmark_summary \
		"$display_name" \
		"$input_size" \
		"$output_size" \
		"$decompressed_size" \
		"$compress_elapsed" \
		"$compress_user" \
		"$compress_system" \
		"$compress_cpu" \
		"$compress_rss" \
		"$decompress_elapsed" \
		"$decompress_user" \
		"$decompress_system" \
		"$decompress_cpu" \
		"$decompress_rss" \
		"$integrity_method" \
		"$original_checksum" \
		"$decompressed_checksum" \
		"$checksum_status" \
		"$roundtrip_status"

	local reduction_percent="0"
	local compression_ratio="0"
	if ((input_size > 0)); then
		reduction_percent=$(awk -v input="$input_size" -v output="$output_size" 'BEGIN { printf("%.2f", (input - output) * 100 / input) }')
		compression_ratio=$(awk -v input="$input_size" -v output="$output_size" 'BEGIN { printf("%.3f", input / output) }')
	fi

	write_metrics_file "$metrics_path" \
		"label=$display_name" \
		"input_size=$input_size" \
		"output_size=$output_size" \
		"decompressed_size=$decompressed_size" \
		"reduction_percent=$reduction_percent" \
		"compression_ratio=$compression_ratio" \
		"compress_elapsed_seconds=$compress_elapsed" \
		"decompress_elapsed_seconds=$decompress_elapsed" \
		"compress_cpu_percent=$compress_cpu" \
		"decompress_cpu_percent=$decompress_cpu" \
		"compress_max_rss_kb=$compress_rss" \
		"decompress_max_rss_kb=$decompress_rss" \
		"checksum_status=$checksum_status" \
		"roundtrip_status=$roundtrip_status"

	remove_empty_dir_if_present "$work_dir"
}

print_comparison_summary() {
	local current_metrics="$1"
	local v1_metrics="$2"

	load_metrics_file current "$current_metrics"
	load_metrics_file v1 "$v1_metrics"

	awk \
		-v current_output_size="$current_output_size" \
		-v v1_output_size="$v1_output_size" \
		-v current_ratio="$current_compression_ratio" \
		-v v1_ratio="$v1_compression_ratio" \
		-v current_reduction="$current_reduction_percent" \
		-v v1_reduction="$v1_reduction_percent" \
		-v current_compress_elapsed="$current_compress_elapsed_seconds" \
		-v v1_compress_elapsed="$v1_compress_elapsed_seconds" \
		-v current_decompress_elapsed="$current_decompress_elapsed_seconds" \
		-v v1_decompress_elapsed="$v1_decompress_elapsed_seconds" \
		-v current_compress_cpu="$current_compress_cpu_percent" \
		-v v1_compress_cpu="$v1_compress_cpu_percent" \
		-v current_decompress_cpu="$current_decompress_cpu_percent" \
		-v v1_decompress_cpu="$v1_decompress_cpu_percent" \
		-v current_compress_rss="$current_compress_max_rss_kb" \
		-v v1_compress_rss="$v1_compress_max_rss_kb" \
		-v current_decompress_rss="$current_decompress_max_rss_kb" \
		-v v1_decompress_rss="$v1_decompress_max_rss_kb" '
BEGIN {
  output_delta = v1_output_size - current_output_size
  printf("\nComparison summary\n")
  printf("  current compressed bytes:   %d\n", current_output_size)
  printf("  spring v1 compressed bytes: %d\n", v1_output_size)
  if (output_delta > 0) {
    printf("  size winner:                current Spring by %d bytes\n", output_delta)
  } else if (output_delta < 0) {
    printf("  size winner:                Spring v1 by %d bytes\n", -output_delta)
  } else {
    printf("  size winner:                tie\n")
  }
  printf("  current compression ratio:  %sx\n", current_ratio)
  printf("  spring v1 compression ratio:%sx\n", v1_ratio)
  printf("  current size reduction:     %s%%\n", current_reduction)
  printf("  spring v1 size reduction:   %s%%\n", v1_reduction)
  if (current_compress_elapsed != "" && v1_compress_elapsed != "") {
    printf("  current compression time:   %ss\n", current_compress_elapsed)
    printf("  spring v1 compression time: %ss\n", v1_compress_elapsed)
  }
  if (current_decompress_elapsed != "" && v1_decompress_elapsed != "") {
    printf("  current decompression time: %ss\n", current_decompress_elapsed)
    printf("  spring v1 decompression time:%ss\n", v1_decompress_elapsed)
  }
  if (current_compress_cpu != "" && v1_compress_cpu != "") {
    printf("  current compression CPU usage:   %s\n", current_compress_cpu)
    printf("  spring v1 compression CPU usage: %s\n", v1_compress_cpu)
  }
  if (current_decompress_cpu != "" && v1_decompress_cpu != "") {
    printf("  current decompression CPU usage: %s\n", current_decompress_cpu)
    printf("  spring v1 decompression CPU usage:%s\n", v1_decompress_cpu)
  }
  if (current_compress_rss != "" && current_compress_rss != "unavailable" && v1_compress_rss != "" && v1_compress_rss != "unavailable") {
    printf("  current peak compression RSS: %s KB\n", current_compress_rss)
    printf("  spring v1 peak compression RSS: %s KB\n", v1_compress_rss)
  }
  if (current_decompress_rss != "" && current_decompress_rss != "unavailable" && v1_decompress_rss != "" && v1_decompress_rss != "unavailable") {
    printf("  current peak decompression RSS: %s KB\n", current_decompress_rss)
    printf("  spring v1 peak decompression RSS: %s KB\n", v1_decompress_rss)
  }
}
'
}

ensure_benchmark_input
ensure_spring_binary
ensure_spring_v1_runner

INPUT_ABS_1=$(realpath -m -- "$INPUT_FASTQ_1")
INPUT_ABS_2=""
if [[ -n "$INPUT_FASTQ_2" ]] && [[ -f "$INPUT_FASTQ_2" ]]; then
	INPUT_ABS_2=$(realpath -m -- "$INPUT_FASTQ_2")
fi

INPUT_BASENAME=$(basename -- "$INPUT_ABS_1")
INPUT_STEM=${INPUT_BASENAME%.*}
MAX_SHORT_READ_LENGTH=511
MAX_READ_LENGTH=$(stream_input_bytes "$INPUT_ABS_1" | awk 'NR % 4 == 2 { if (length($0) > max_len) max_len = length($0) } END { print max_len + 0 }')

run_benchmark "current" "Current Spring" "$SPRING_BIN"
run_benchmark "spring_v1" "Spring v1" "$SPRING_V1_RUNNER"
print_comparison_summary "$TMP_LOG_DIR/current.metrics" "$TMP_LOG_DIR/spring_v1.metrics"
