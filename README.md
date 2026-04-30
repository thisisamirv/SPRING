<!-- markdownlint-disable MD033 MD034 -->
# SPRING2

<p align="center">
  <a href="https://github.com/thisisamirv/SPRING2/actions/workflows/ci.yml">
    <img src="https://github.com/thisisamirv/SPRING2/actions/workflows/ci.yml/badge.svg" alt="CI">
  </a>
</p>

<p align="center">
  <img src="assets/icons/logo.png" alt="SPRING2 logo" width="128" />
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
- Record-level CRC32 verification ensures 100% data fidelity for lossless archives.

## Platform Support

SPRING2 is built and validated on:

- **Linux**: x86_64, ARM64
- **macOS**: Universal (Intel/Apple Silicon)
- **Windows**: Native via MinGW-w64

## Documentation

Detailed documentation is available in the `docs/` directory:

- **[Installation & Building](docs/BUILDING.md)**: System requirements and platform-specific build instructions.
- **[Usage Guide](docs/USAGE.md)**: Command-line options, configuration, and practical examples.
- **[Development & Maintenance](docs/DEVELOPMENT.md)**: Information for contributors on project structure, linting, and internal architecture.

## Installation

### Pre-built Binaries

The easiest way to use SPRING2 is to download the pre-built binaries from the **[Releases](https://github.com/thisisamirv/SPRING/releases)** page.

## Running SPRING

Current command-line help:

```text
Allowed options:

* General Options:
  -h [ --help ]                   produce help message
  -V [ --version ]                produce version information
  -o [ --output ] arg             output file name
                                    - if not specified, it uses original input
                                      filenames (swapping extension to .sp during
                                      compression)
                                    - for paired end decompression, if only one file
                                      is specified, two output files will be created
                                      by suffixing .1 and .2
  -w [ --tmp-dir ] arg (=.)       directory to create temporary files (default
                                  current directory)
  -t [ --threads ] arg            number of threads (default:
                                  min(max(1, hw_threads - 1), 16))
  -m [ --memory ] arg (=0)        approximate memory budget in GB; reduces
                                  effective thread count using about 1 GB per
                                  worker thread (0 disables)
  -v [ --verbose ]                enable extensive logging (default: progress bar)
--------------------------------------------------------------------------------
* Compression Options:
  -c [ --compress ]               compress
  -R1 [ --R1 ] arg                input read-1 file (required)
  -R2 [ --R2 ] arg                input read-2 file (optional; enables paired-end mode)
  -R3 [ --R3 ] arg                input read-3 file (optional; requires --R2)
  -I1 [ --I1 ] arg                input index-read-1 file (optional; requires --R2)
  -I2 [ --I2 ] arg                input index-read-2 file (optional; requires --I1)
  -l [ --level ] arg (=6)         compression level (1-9) to use for output
                                  (.gz) formatting (passed to gzip unchanged
                                  and scaled to Zstd 1-22 internally)
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
  -n [ --note ] arg               add a custom note to the archive
  -y [ --assay ] arg (=auto)      specify assay type. Valid choices:
                                  auto, dna, rna, atac, bisulfite,
                                  sc-rna, sc-atac, sc-bisulfite
  -a [ --audit ]                  enable post-operation integrity verification
--------------------------------------------------------------------------------
* Decompression Options:
  -d [ --decompress ]             decompress
  -i [ --input ] arg              input archive file (.sp)
  -u [ --unzip ]                  during decompression, force output to be
                                  uncompressed (even if original was .gz)

When an archive was created with grouped lanes (`--R3` and/or `--I1/--I2`),
decompression with a single `-o <prefix>` emits grouped outputs using suffixes
`.R1`, `.R2`, optional `.R3`, optional `.I1`, and optional `.I2`.
```

## Previewing Archives

You can use `spring2 --preview` to quickly inspect archive metadata, original filenames, and custom notes. It also provides a high-speed **Audit** mode to verify archive integrity without full decompression:

```bash
# Display metadata preview
spring2 --preview archive.sp

# Perform a high-speed integrity audit (dry-run)
spring2 --preview --audit archive.sp
```

Example:

```text
--------------------------------
Original Input 1:  file.fastq.gz
Note:              My Note
Assay Type:        sc-rna
Mode:              Single-end
Reads Processed:   321055
Compression Ratio: 5.81x (2312 / 398 MB)
Max Read Length:   301 (using short-read encoder)
Preserve Order:    Yes
Preserve IDs:      Yes
Preserve Quality:  Yes
Quality Mode:      Lossless
Compression Level: 6
Use CRLF:          No
--------------------------------
Input 1 Original Compression:
  Profile:         BGZF (Default)
  Format:          BGZF (Block Gzip)
  Block Size:      2185
  Uncompressed Name: file.fastq
  Gzip Header:     FLG=0x4, MTIME=0, OS=255
  Member Count:    102860
  Original Ratio:  5.53x (6382 / 1154 MB)
  Likely Origin:   htslib/samtools/clib
--------------------------------
```

For a comprehensive list of all options, quality modes, and multi-threaded examples, see the **[Usage Guide](docs/USAGE.md)**.

## Related

- QVZ: <https://github.com/mikelhernaez/qvz/>
- [Bioinformatics publication](https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty1015/5232998?guestAccessKey=266a1378-4684-4f04-bb99-6febdf9d1fb9)
- Specialized tool for nanopore long reads: <https://github.com/qm2/NanoSpring>

## Reporting Bugs and Errors

If you encounter a bug, crash, or unexpected behavior, please open an issue at:

<https://github.com/thisisamirv/SPRING2/issues>

To help us reproduce and diagnose problems quickly, include:

- SPRING2 version (`spring2 --version`)
- Platform details (OS, architecture, shell/environment)
- Exact command you ran

For diagnostics, please also provide logs from debug verbosity. Capture both stdout and stderr into a file:

```bash
spring2 ... --verbose debug > spring2-debug.log 2>&1
```

PowerShell equivalent:

```powershell
spring2 ... --verbose debug *> spring2-debug.log
```

Please upload `spring2-debug.log` to the issue so we can diagnose the problem quickly.

If your command includes sensitive paths or data, redact private information before posting logs.
