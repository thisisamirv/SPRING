# SPRING2 Compression Guide

This guide focuses on archive creation.

## Basic Syntax

```bash
spring2 -c --R1 <input_R1> [--R2 <input_R2>] [--R3 <input_R3>] \
  [--I1 <input_I1>] [--I2 <input_I2>] -o <output.sp>
```

## Supported Compression Inputs

- Single-end FASTQ
- Paired-end FASTQ
- FASTA inputs
- Gzipped inputs ending in `.gz`
- Grouped lane sets with optional read-3 and index lanes

SPRING2 auto-detects FASTQ versus FASTA and auto-detects gzipped input files.

## Common Examples

Single-end:

```bash
spring2 -c --R1 reads.fastq -o reads.sp
```

Paired-end:

```bash
spring2 -c --R1 reads_1.fastq --R2 reads_2.fastq -o reads.sp
```

Gzipped paired-end:

```bash
spring2 -c --R1 reads_1.fastq.gz --R2 reads_2.fastq.gz -o reads.sp
```

Grouped paired-end with indexes:

```bash
spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
```

Grouped paired-end with read-3 and indexes:

```bash
spring2 -c --R1 R1.fastq.gz --R2 R2.fastq.gz --R3 R3.fastq.gz --I1 I1.fastq.gz --I2 I2.fastq.gz -o run.sp
```

## Grouped Lanes

Grouped archives are created when you pass `--R3` and/or `--I1` / `--I2`.

Rules:

- `--R3` requires `--R2`
- `--I1` currently requires `--R2`
- `--I2` requires `--I1`

Grouped archives preserve all provided lanes together and restore them with a
single decompression command.

## Performance Controls

### Threads

Use `--threads` to control concurrency:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --threads 8
```

The default thread count is `min(max(1, hw_threads - 1), 16)`.

### Memory Budget

Use `--memory` to cap effective worker concurrency based on an approximate
memory budget:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --memory 8
```

Set `--memory 0` to disable this safety knob.

### Compression Level

Use `--level 1..9` to change the archive compression level used for gzip-style
formatting and the internal scaling used for other streams:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --level 9
```

## Notes and Metadata

Attach a custom note with `--note`:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --note "batch=07 replicate=A"
```

That note is stored in archive metadata and shown by preview mode.

## Compression-Time Verification

Add `--audit` to verify the archive immediately after compression:

```bash
spring2 -c --R1 reads.fastq --R2 reads_2.fastq -o reads.sp --audit
```

This runs a dry-run verification pass after archive creation.

## Logging

- Default: progress bar plus warnings and errors
- `--verbose` or `--verbose info`: informational logs
- `--verbose debug`: detailed diagnostics

Example:

```bash
spring2 -c --R1 reads.fastq -o reads.sp --verbose debug
```

## See Also

- [Assays and Quality Modes](ASSAYS_AND_QUALITY.md)
- [CLI Reference](CLI_REFERENCE.md)
