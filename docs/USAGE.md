# SPRING2 Usage Guide

This guide provides detailed information on how to use SPRING2 for compressing and decompressing sequencing data.

## Basic Commands

The general syntax for SPRING2 is:

```bash
spring2 [mode] [options] --R1 <input_R1> [--R2 <input_R2>] [--R3 <input_R3>] [--I1 <input_I1>] [--I2 <input_I2>] -o <output_file>
```

### Modes

- `-c, --compress`: Perform compression.
- `-d, --decompress`: Perform decompression.
- `-u, --unzip`: During decompression, force the output to be uncompressed (plain FASTQ/FASTA), even if the original input was a `.gz` file.

### Decompression Input

- `-i, --input arg`: Archive input file for decompression. Provide one `.sp` file.

## Compression Options

### Input and Output

- `-R1, --R1 arg`: Read 1 input file. Required for compression.
- `-R2, --R2 arg`: Read 2 input file. Optional; if provided, SPRING2 automatically switches to paired-end mode.
- `-R3, --R3 arg`: Read 3 input file. Optional; requires `--R2`.
- `-I1, --I1 arg`: Index read 1 input file. Optional; requires `--R2`.
- `-I2, --I2 arg`: Index read 2 input file. Optional; requires `--I1`.
- `-o, --output arg`: Output file name. If omitted, SPRING2 will use the original input name(s) (e.g., swapping extensions to `.sp` during compression). For paired-end decompression, if only one file is specified, two output files will be created by suffixing `.1` and `.2`.

When grouped lane options are provided (`--R3` and/or `--I1/--I2`), SPRING2
stores a grouped archive containing the read pair (`R1/R2`) plus optional
read-3 and index lanes. During decompression, `-o <prefix>` expands to
`<prefix>.R1`, `<prefix>.R2`, optional `<prefix>.R3`, optional `<prefix>.I1`,
and optional `<prefix>.I2`.

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
- `-n, --note arg`: Attach a custom text note to the archive. This note is stored in the metadata and can be viewed during decompression or with `spring2 --preview`.
- `-y, --assay arg`: Specify the sequencing assay type (`auto`, `dna`, `rna`, `atac`, `bisulfite`, `sc-rna`, `sc-atac`, `sc-bisulfite`). Default is `auto`. The assay type is stored in the metadata and viewable with `spring2 --preview`. Note: `dna` is used interchangeably for standard Whole Genome Sequencing (WGS) and ChIP-seq (`chip`), as their read structure and formatting are effectively identical.
- `-b, --cb-len arg`: Cellular barcode length in bases (integer, 1–64). Used when `--assay` is `sc-rna`, `sc-atac`, or `sc-bisulfite` and no I1 index lane is provided. When an I1 lane is present with `--I1`, the CB length is auto-detected from the I1 read length and this flag is ignored. Default: `16`.
- `-v, --verbose [info|debug]`: Control logging verbosity.
  - Default (no `--verbose`): shows the concise stage-based progress bar, plus warnings/errors.
  - `--verbose` or `--verbose info`: disables the progress bar and prints informational step-by-step logs.
  - `--verbose debug`: includes all informational logs plus detailed debug diagnostics for troubleshooting.

### Quality Modes (`-q, --qmod`)

1. **lossless** (Default): Bit-perfect retention of quality scores.
2. **qvz qv_ratio**: Lossy compression using [QVZ](https://github.com/mikelhernaez/qvz/). The `qv_ratio` roughly corresponds to the bits used per quality value.
3. **ill_bin**: Illumina 8-level binning.
4. **binary thr high low**: Binary (2-level) thresholding. Qualities are binned to `high` if >= `thr`, and to `low` if < `thr`.

## Examples

### Paired-End FASTQ (Lossless)

```bash
./spring2 -c --R1 file_1.fastq --R2 file_2.fastq -o file.sp -n "Batch 01 - Control"
```

### Paired-End FASTQ + Index Reads

```bash
./spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
./spring2 -d -i run.sp -o run_out
# Produces: run_out.R1 run_out.R2 run_out.I1 run_out.I2
```

### Paired-End FASTQ + Read3 + Index Reads

```bash
./spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --R3 R3.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
./spring2 -d -i run.sp -o run_out
# Produces: run_out.R1 run_out.R2 run_out.R3 run_out.I1 run_out.I2
```

### Verbose Logging Levels

```bash
# Informational logs (same as --verbose info)
./spring2 -c --R1 file_1.fastq -o file.sp --verbose

# Explicit info level
./spring2 -d -i file.sp --verbose info

# Deep debug diagnostics for issue reporting
./spring2 -d -i file.sp --verbose debug
```

### Inspected with Preview Mode

To view the metadata and notes of an archive without decompressing it:

```bash
./spring2 --preview file.sp
```

To check the version of either tool:

```bash
./spring2 --version
./spring2 --version
```

### Paired-End Gzipped FASTQ

SPRING2 automatically detects gzipped inputs and decompresses them on the fly using internal `rapidgzip` (if available) or `zlib`. Detailed gzip metadata (profile, block size, original internal filename) is stored in the archive for high-fidelity restoration.

```bash
./spring2 -c --R1 file_1.fastq.gz --R2 file_2.fastq.gz -o file.sp
```

### Lossy Compression (QVZ)

```bash
./spring2 -c --R1 file_1.fastq --R2 file_2.fastq -s oi -q qvz 1.0 -o file.sp
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

## Archive Integrity Auditing

SPRING2 implements a robust, logical record-level hashing system to guarantee 100% data fidelity for lossless archives.

### Mandatory Verification

When you decompress an archive using `spring2 -d`, the program automatically recalculates CRC32 digests for all sequences, identifiers, and quality scores. These are compared against the golden digests stored during compression. If any discrepancy is found, decompression will abort with an integrity error.

### Proactive Auditing (Dry-Run)

You can audit an archive's integrity without performing a full decompression to disk. This is useful for verifying cold storage or large transfers where disk space for full extraction is not immediately available.

**Standalone Audit**: Use the `-a, --audit` flag with `spring2 --preview`:

```bash
spring2 --preview --audit archive.sp
```

**Post-Compression Audit**: You can add `-a, --audit` to your standard `spring2` compression command to perform a verification pass immediately after the archive is created:

```bash
spring2 -c -i input.fastq -o archive.sp -a
```

This will perform the compression normally, and then immediately run an internal dry-run decompression to verify the final archive's integrity before finishing. (Note: Decompression already performs this verification automatically).

This "dry-run" mode reconstructs all records in memory and verifies their integrity, then reports success or failure. It is significantly faster than a full decompression because it eliminates all disk I/O overhead for reconstructed FASTQ files.

## Archive Format

SPRING2 archives (`.sp`) are technically tar files containing several compressed internal streams (reads, quality, IDs, metadata). You can technically inspect them with `tar -tf file.sp`, but they should always be evaluated using `spring2`, especially `spring2 --preview`, to ensure data integrity.
