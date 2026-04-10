# SPRING2 Usage Guide

This guide provides detailed information on how to use SPRING2 for compressing and decompressing sequencing data.

## Basic Commands

The general syntax for SPRING2 is:

```bash
spring2 [mode] [options] -i <input_files> -o <output_file>
```

### Modes

- `-c, --compress`: Perform compression.
- `-d, --decompress`: Perform decompression.
- `-u, --unzip`: During decompression, force the output to be uncompressed (plain FASTQ/FASTA), even if the original input was a `.gz` file.

## Compression Options

### Input and Output

- `-i, --input arg`: Input file name. Specify two files for paired-end data.
- `-o, --output arg`: Output file name. If omitted, SPRING2 will use the original input name(s) (e.g., swapping extensions to `.sp` during compression). For paired-end decompression, if only one file is specified, two output files will be created by suffixing `.1` and `.2`.

### Performance and Resources

- `-t, --threads arg`: Number of threads to use. Defaults to `min(max(1, hw_threads - 1), 16)`.
- `-m, --memory arg`: Approximate memory budget in GB. This is a safety knob that reduces the effective worker-thread count using an approximate budget of about 1 GB per worker thread. Set to `0` to disable.
- `-w, --tmp-dir arg`: Directory to create temporary files. Defaults to the current directory.

### Data Manipulation

- `-s, --strip arg`: Discard specific data components:
  - `i`: Read identifiers
  - `o`: Original read order
  - `q`: Quality scores
  - *Example*: `--strip io` to drop both identifiers and original order.
- `-l, --level arg`: Compression level (1-9). This controls the compression level for `.gz` file formatting and is scaled to Zstd (1-22) internally for other streams. Default is `6`.
- `-n, --note arg`: Attach a custom text note to the archive. This note is stored in the metadata and can be viewed during decompression or using the preview tool.
- `-v, --verbose`: Enable extensive logging. By default, SPRING2 displays a concise, stage-based progress bar. Enabling verbose mode disables the progress bar and prints detailed step-by-step logs, including data statistics and execution times.

### Quality Modes (`-q, --qmod`)

1. **lossless** (Default): Bit-perfect retention of quality scores.
2. **qvz qv_ratio**: Lossy compression using [QVZ](https://github.com/mikelhernaez/qvz/). The `qv_ratio` roughly corresponds to the bits used per quality value.
3. **ill_bin**: Illumina 8-level binning.
4. **binary thr high low**: Binary (2-level) thresholding. Qualities are binned to `high` if >= `thr`, and to `low` if < `thr`.

## Examples

### Paired-End FASTQ (Lossless)

```bash
./spring2 -c -i file_1.fastq file_2.fastq -o file.sp -n "Batch 01 - Control"
```

### Inspected with Preview Tool

To view the metadata and notes of an archive without decompressing it:

```bash
./spring2-preview file.sp
```

### Paired-End Gzipped FASTQ

SPRING2 automatically detects gzipped inputs and decompresses them on the fly using internal `rapidgzip` (if available) or `zlib`. Detailed gzip metadata (profile, block size, original internal filename) is stored in the archive for high-fidelity restoration.

```bash
./spring2 -c -i file_1.fastq.gz file_2.fastq.gz -o file.sp
```

### Lossy Compression (QVZ)

```bash
./spring2 -c -i file_1.fastq file_2.fastq -s oi -q qvz 1.0 -o file.sp
```

### Decompression with Gzipped Output

SPRING2 infers output formatting from the extension. If you specify an output filename ending in `.gz`, the output will be gzipped using the compression parameters (or defaults) stored in the archive.

```bash
./spring2 -d -i file.sp -o file_1.fastq.gz file_2.fastq.gz
```

### Force Uncompressed Output

If the original input was gzipped, SPRING2 will default to producing gzipped output during decompression (to maintain symmetry). Use the `-u` flag to force uncompressed output:

```bash
./spring2 -d -u -i file.sp
```

## Archive Format

SPRING2 archives (`.sp`) are technically tar files containing several compressed internal streams (reads, quality, IDs, metadata). You can technically inspect them with `tar -tf file.sp`, but they should always be manipulated using the `spring2` executable to ensure data integrity.
