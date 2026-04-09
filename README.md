<!-- markdownlint-disable MD033 MD034 -->
# SPRING2

<p align="center">
  <a href="https://github.com/thisisamirv/Spring/actions/workflows/ci.yml">
    <img src="https://github.com/thisisamirv/Spring/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

> **LICENSE RESTRICTION NOTICE**
>
> **SPRING is not provided under a permissive open-source license. Under the Non-Exclusive Research Use Software and Patent License Agreement, the software and patent rights are licensed only for non-profit research, educational, personal, and individual use. Commercial use, sublicensing, assignment, transfer, or other unlicensed making-available to third parties is not authorized absent a separate license from the University of Illinois. Any use, copying, modification, disclosure, or redistribution must comply with the original license terms, and derivative works must be clearly marked and renamed. Treat this repository as demonstration and reference material unless your intended use is expressly permitted by that license.**

SPRING2 is a compressor for FASTQ and FASTA sequencing data, including paired-end data and gzipped FASTQ inputs. It is built on top of the original **[SPRING](https://github.com/shubhamchandak94/SPRING)** project and represents a substantial modernization of that codebase.

> [!TIP]
> This revision is a substantial modernization of the original SPRING codebase. In brief, it upgrades the build baseline to C++20 and CMake 4.2, removes the remaining Boost dependency, adds cross-platform build and CI support including Windows via MSYS2 UCRT64, auto-detects FASTA and gzipped compression inputs, infers gzipped decompression output from the output filename, adds the `--memory` safety knob, and introduces cleaner developer tooling and benchmark scripts alongside broader portability, lint, and reliability fixes.

## Features

- Near-optimal compression ratios for single-end and paired-end datasets
- Fast and memory-efficient decompression
- Lossless compression of reads, quality scores, and read identifiers
- Optional quality quantization using [QVZ](https://github.com/mikelhernaez/qvz/), [Illumina 8-level binning](https://www.illumina.com/documents/products/whitepapers/whitepaper_datacompression.pdf), or binary thresholding
- Automatic FASTQ versus FASTA input detection
- Gzipped FASTQ input support and gzip output on decompression based on output filename
- Free for non-profit research and educational use
- Automatic detection of short-read (up to 511 bases) and long-read modes

## Platform Support

SPRING2 is built and validated on:

- **Linux**: x86_64, ARM64
- **macOS**: Universal (Intel/Apple Silicon)
- **Windows**: Native via MinGW-w64

## Installation

### Pre-built Binaries

The easiest way to use SPRING2 is to download the pre-built binaries from the **[Releases](https://github.com/thisisamirv/SPRING/releases)** page.

- **Linux**: Portable AppImages are available for both x86_64 and ARM64.
- **macOS**: A universal binary is provided for all modern Mac hardware.
- **Windows**: Standalone `.exe` binaries are available.

### Building from Source

If you prefer to build SPRING2 yourself, please refer to our **[BUILD.md](BUILD.md)** file for a full list of dependencies and platform-specific build instructions.

## Running SPRING

The built executable is:

```bash
build/spring
```

Current command-line help:

```text
Allowed options:
  -h [ --help ]                   produce help message
  -c [ --compress ]               compress
  -d [ --decompress ]             decompress
  -i [ --input ] arg              input file name (two files for paired end)
  -o [ --output ] arg             output file name (for paired end
                                  decompression, if only one file is specified,
                                  two output files will be created by suffixing
                                  .1 and .2.)
  -w [ --tmp-dir ] arg (=.)       directory to create temporary files (default
                                  current directory)
  -t [ --threads ] arg            number of threads (default:
                                  min(max(1, hw_threads - 1), 16))
  -m [ --memory ] arg (=0)        approximate memory budget in GB; reduces
                                  effective thread count using about 1 GB per
                                  worker thread (0 disables)
  -s [ --strip ] arg              discard data: i (ids), o (order), q (quality)
                                  Example: --strip io to drop ids and order.
  -q [ --qmod ] arg               quality mode: possible modes are
                                  1. -q lossless (default)
                                  2. -q qvz qv_ratio (QVZ lossy compression,
                                  parameter qv_ratio roughly corresponds to
                                  bits used per quality value)
                                  3. -q ill_bin (Illumina 8-level binning)
                                  4. -q binary thr high low (binary (2-level)
                                  thresholding, quality binned to high if >=
                                  thr and to low if < thr)
  -l [ --level ] arg (=6)         compression level (1-9) to use for output
                                  (.gz) formatting (passed to gzip unchanged
                                  and scaled to Zstd 1-22 internally)
```

SPRING2 archives are tar files containing the internal compressed streams, though using a `.sp` extension is recommended.

`--memory` is a conservative safety knob for machines with many cores and limited RAM. It does not hard-limit total allocation. Instead, it reduces the effective worker-thread count using an approximate budget of about 1 GB per worker thread.

## Smoke Tests And Lint

Run the basic smoke tests:

```bash
tests/test_script.sh
```

Run the project lint wrapper:

```bash
./dev/lint.sh
```

On macOS analysis runs, CI prepends Homebrew LLVM to `PATH` before linting.

## Resource Usage

For memory and CPU performance numbers, see the paper and supplementary material. SPRING2 also uses temporary disk space during compression.

In short-read mode, when qualities and identifiers are retained:

- default lossless mode typically uses temporary disk space around 10% to 30% of the original uncompressed input
- `-s o` mode can push temporary disk usage much higher, often around 70% to 80% of the original file size

These figures are approximate and include the space needed for the final compressed output.

## Example Usage

Compress paired-end FASTQ losslessly:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -o file.sp
```

Compress gzipped paired-end FASTQ losslessly:

```bash
./spring2 -c -i file_1.fastq.gz file_2.fastq.gz -o file.sp
```

Compress with 16 threads:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -o file.sp -t 16
```

Compress with Illumina binning and no stored identifiers:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -s oi -q ill_bin -o file.sp
```

Compress with binary-thresholded qualities:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -s oi -q binary 20 40 6 -o file.sp
```

Compress with QVZ quantization:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -s oi -q qvz 1.0 -o file.sp
```

Compress reads and identifiers only:

```bash
./spring2 -c -i file_1.fastq file_2.fastq -s q -o file.sp
```

Compress single-end data without preserving order:

```bash
./spring2 -c -i file.fastq -s o -o file.sp
```

Decompress single-end data:

```bash
./spring2 -d -i file.sp -o file.fastq
```

Decompress paired-end data to suffixed outputs:

```bash
./spring2 -d -i file.sp -o file.fastq
```

Decompress paired-end data to explicit outputs:

```bash
./spring2 -d -i file.sp -o file_1.fastq file_2.fastq
```

Decompress paired-end data directly to gzip outputs:

```bash
./spring2 -d -i file.sp -o file_1.fastq.gz file_2.fastq.gz
```

Compress paired-end FASTA losslessly:

```bash
./spring2 -c -i file_1.fasta file_2.fasta -o file.sp
```

Decompress paired-end FASTA:

```bash
./spring2 -d -i file.sp -o file_1.fasta file_2.fasta
```

## Related

- QVZ: <https://github.com/mikelhernaez/qvz/>
- [Bioinformatics publication](https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty1015/5232998?guestAccessKey=266a1378-4684-4f04-bb99-6febdf9d1fb9)
- Specialized tool for nanopore long reads: <https://github.com/qm2/NanoSpring>
