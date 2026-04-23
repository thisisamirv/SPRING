#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
INPUT_DIR="$ROOT_DIR/assets/sample-data"
OUTPUT_BASE="$SCRIPT_DIR/output"
LOG_DIR="$OUTPUT_BASE/logs"
OUTPUT_DIR="$OUTPUT_BASE/runs"
WORK_ROOT_DIR="$OUTPUT_BASE/work"
SPRING_BIN=${SPRING_BIN:-"$ROOT_DIR/build/spring2"}
SPRING_PREVIEW_BIN=${SPRING_PREVIEW_BIN:-"$ROOT_DIR/build/spring2-preview"}
INPUT_FASTQ=${1:-"$INPUT_DIR/sample.fastq"}
THREADS=${THREADS:-8}
BUILD_DIR="$ROOT_DIR/build"
BUILD_LOG="$LOG_DIR/build.log"
COMPRESS_RESOURCE_LOG="$LOG_DIR/compress_resource_usage.log"
DECOMPRESS_RESOURCE_LOG="$LOG_DIR/decompress_resource_usage.log"

remove_empty_dir_if_present() {
	local dir_path="$1"

	if [[ -d "$dir_path" ]] && [[ -z "$(find "$dir_path" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
		rmdir "$dir_path"
	fi
}

trap 'remove_empty_dir_if_present "$WORK_ROOT_DIR"' EXIT

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

ensure_spring_binary() {
	if [[ -x "$SPRING_BIN" ]]; then
		return
	fi

	mkdir -p "$LOG_DIR"

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

# --- Main Logic ---

run_single_file_benchmark() {
    local input_path="$1"
    INPUT_ABS=$(realpath -m -- "$input_path")
    INPUT_BASENAME=$(basename -- "$INPUT_ABS")
    INPUT_STEM=${INPUT_BASENAME%.*}
    WORK_DIR="$WORK_ROOT_DIR/$INPUT_STEM.work"
    OUTPUT_FILE="$OUTPUT_DIR/$INPUT_STEM.sp"
    DECOMPRESSED_OUTPUT_FILE="$OUTPUT_DIR/$INPUT_STEM.roundtrip.fastq"

    echo -e "\n=== Benchmarking $INPUT_BASENAME ==="
    
    MAX_READ_LENGTH=$(awk 'NR % 4 == 2 { if (length($0) > max_len) max_len = length($0) } END { print max_len + 0 }' "$INPUT_ABS")

    mkdir -p "$INPUT_DIR" "$LOG_DIR" "$OUTPUT_DIR" "$WORK_ROOT_DIR"
    rm -rf "$WORK_DIR"
    mkdir -p "$WORK_DIR"
    rm -f "$OUTPUT_FILE"
    rm -f "$DECOMPRESSED_OUTPUT_FILE"

    echo "Running Spring lossless compression (auto-assay)"
    spring_args=(
        -c
        --R1 "$INPUT_ABS"
        -o "$OUTPUT_FILE"
        -w "$WORK_DIR"
        -t "$THREADS"
        -q lossless
        --assay auto
    )
    run_with_resource_log "$COMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${spring_args[@]}"

    echo "Running Spring decompression"
    decompress_args=(
        -d
        -i "$OUTPUT_FILE"
        -o "$DECOMPRESSED_OUTPUT_FILE"
        -w "$WORK_DIR"
    )
    run_with_resource_log "$DECOMPRESS_RESOURCE_LOG" "$SPRING_BIN" "${decompress_args[@]}"

    INPUT_SIZE=$(stat -c%s "$INPUT_ABS")
    OUTPUT_SIZE=$(stat -c%s "$OUTPUT_FILE")
    
    roundtrip_status="different"
    if [[ "$INPUT_ABS" == *.gz ]]; then
        if cmp -s <(gzip -dc "$INPUT_ABS") "$DECOMPRESSED_OUTPUT_FILE"; then
            roundtrip_status="identical"
        fi
    else
        if cmp -s "$INPUT_ABS" "$DECOMPRESSED_OUTPUT_FILE"; then
            roundtrip_status="identical"
        fi
    fi

    echo "  Results for $INPUT_BASENAME"
    echo "    Compressed size: $OUTPUT_SIZE bytes"
    echo "    Bit-perfect:     $roundtrip_status"
}

run_assay_suite() {
    echo -e "\n--- Running Assay Benchmark Suite ---"

    samples=(
        "Methylation (test_3);test_3_R1.fastq.gz;test_3_R2.fastq.gz;;;methyl"
        "sc-ATAC (test_4);test_4_R1.fastq.gz;test_4_R2.fastq.gz;test_4_R3.fastq.gz;test_4_I1.fastq.gz;sc-atac"
        "sc-RNA (test_5);test_5_R1.fastq.gz;test_5_R2.fastq.gz;test_5_I1.fastq.gz;test_5_I2.fastq.gz;sc-rna"
    )

    for s in "${samples[@]}"; do
        IFS=';' read -r name r1 r2 r3 i1 assay <<< "$s"
        
        if [[ ! -f "$INPUT_DIR/$r1" ]]; then continue; fi
        echo -e "\n>>> Assay: $name"

        work="$WORK_ROOT_DIR/${r1%.*}.bench"
        rm -rf "$work" && mkdir -p "$work"
        out_auto="$OUTPUT_DIR/${r1%.*}.auto.sp"
        out_dna="$OUTPUT_DIR/${r1%.*}.dna.sp"

        files=("$r1")
        [[ -n "$r2" ]] && files+=("$r2")
        [[ -n "$r3" ]] && files+=("$r3")
        [[ -n "$i1" ]] && files+=("$i1")
        [[ -n "$i2" ]] && files+=("$i2")

        base_args=()
        for f in "${files[@]}"; do
            abs="$INPUT_DIR/$f"
            # Extract R1, R2, etc. from filename
            key=$(echo "$f" | grep -oP '(R|I)[0-9]')
            base_args+=("--$key" "$abs")
        done

        # 1. Auto-detected assay
        echo "  Step 1: Compression with --assay auto (expected: $assay)"
        "$SPRING_BIN" -c "${base_args[@]}" -o "$out_auto" -w "$work" -t "$THREADS" -q lossless --assay auto
        size_auto=$(stat -c%s "$out_auto")

        # 2. Restoration check (Full Round-Trip)
        echo "  Step 2: Verifying bit-perfect restoration..."
        decomp_dir="$work/decomp"
        mkdir -p "$decomp_dir"
        decomp_files=()
        for f in "${files[@]}"; do
            decomp_files+=("$decomp_dir/${f%.gz}")
        done
        "$SPRING_BIN" -d -i "$out_auto" -o "${decomp_files[@]}" -w "$work"

        all_identical=true
        for i in "${!files[@]}"; do
            orig="$INPUT_DIR/${files[$i]}"
            restored="${decomp_files[$i]}"
            if ! cmp -s <(gzip -dc "$orig") "$restored"; then
                echo "    Mismatch in ${files[$i]}!"
                all_identical=false
            fi
        done

        if [ "$all_identical" = true ]; then
            echo "    Bit-perfect: YES"
        else
            echo "    Bit-perfect: NO"
        fi

        # 3. DNA-mode comparison
        echo "  Step 3: Compression with --assay dna"
        "$SPRING_BIN" -c "${base_args[@]}" -o "$out_dna" -w "$work" -t "$THREADS" -q lossless --assay dna
        size_dna=$(stat -c%s "$out_dna")

        # 4. Results
        gain=$(( (size_dna - size_auto) * 100 / size_dna ))
        echo -e "\n  Assay-specific Optimization Results:"
        echo "    Auto-detected size ($assay): $size_auto bytes"
        echo "    Generic DNA-mode size:       $size_dna bytes"
        echo "    Optimization Gain:           $gain%"
    done
}

if [[ $# -eq 0 ]] && [[ -f "$INPUT_DIR/test_3_R1.fastq.gz" ]]; then
    run_assay_suite
else
    run_single_file_benchmark "$INPUT_FASTQ"
fi
