# SPRING

> **LICENSE RESTRICTION NOTICE**
>
> **SPRING is not provided under a permissive open-source license. Under the Non-Exclusive Research Use Software and Patent License Agreement, the software and patent rights are licensed only for non-profit research, educational, personal, and individual use. Commercial use, sublicensing, assignment, transfer, or other unlicensed making-available to third parties is not authorized absent a separate license from the University of Illinois. Any use, copying, modification, disclosure, or redistribution must comply with the original license terms, and derivative works must be clearly marked and renamed. Treat this repository as demonstration and reference material unless your intended use is expressly permitted by that license.**

[![CI](https://github.com/thisisamirv/Spring/actions/workflows/ci.yml/badge.svg)](https://github.com/thisisamirv/Spring/actions/workflows/ci.yml)

SPRING is a compressor for FASTQ and FASTA sequencing data, including paired-end data and gzipped FASTQ inputs.

> [!TIP]
> This revision is a substantial modernization of the original SPRING codebase. In brief, it upgrades the build baseline to C++20 and CMake 4.2, removes the remaining Boost dependency, adds cross-platform build and CI support including Windows via MSYS2 UCRT64, auto-detects FASTA and gzipped compression inputs, infers gzipped decompression output from the output filename, adds the `--memory-cap-gb` safety knob, and introduces cleaner developer tooling and benchmark scripts alongside broader portability, lint, and reliability fixes.

## Features

- Near-optimal compression ratios for single-end and paired-end datasets
- Fast and memory-efficient decompression
- Lossless compression of reads, quality scores, and read identifiers
- Optional quality quantization using [QVZ](https://github.com/mikelhernaez/qvz/), [Illumina 8-level binning](https://www.illumina.com/documents/products/whitepapers/whitepaper_datacompression.pdf), or binary thresholding
- Automatic FASTQ versus FASTA input detection
- Gzipped FASTQ input support and gzip output on decompression based on output filename
- Random-access decompression with `--decompress-range`
- Short-read mode for reads up to 511 bases
- Long-read mode with `-l` for arbitrarily long reads

## Platform Support

The current CI workflow validates SPRING on:

- Linux
- macOS
- Windows via MSYS2 UCRT64

The build system currently requires:

- A C++20-capable compiler
- CMake 4.2 or newer
- Ninja
- NASM
- OpenMP

## Source Build

Clone the repository:

```bash
git clone https://github.com/thisisamirv/SPRING.git
cd SPRING
```

CI builds use `-Dspring_optimize_for_native=OFF -Dspring_optimize_for_portability=ON` for portability. For local builds on a single machine, the default native-tuned build is fine. If you need a more portable binary, pass the same portability flags used in CI.

### Linux

Install the build requirements:

```bash
sudo apt-get update
sudo apt-get install -y build-essential libomp-dev nasm ninja-build python3 python3-pip python3-venv
python3 -m venv .cmake-venv
. .cmake-venv/bin/activate
python -m pip install --upgrade pip
python -m pip install "cmake==4.2.0"
```

Configure and build:

```bash
cmake -S . -B build \
  -G Ninja \
  -Dspring_optimize_for_native=OFF \
  -Dspring_optimize_for_portability=ON
cmake --build build --parallel
```

Optional analysis tools used in CI:

```bash
sudo apt-get install -y clang-tidy cppcheck valgrind
```

### macOS

Install Xcode Command Line Tools first if needed:

```bash
xcode-select --install
```

Install Homebrew packages:

```bash
brew update
brew install libomp nasm ninja python llvm cppcheck
python3 -m venv .cmake-venv
. .cmake-venv/bin/activate
python -m pip install --upgrade pip
python -m pip install "cmake==4.2.0"
```

Configure and build with Apple Clang and Homebrew `libomp`:

```bash
MACOS_LIBOMP_PREFIX="$(brew --prefix libomp)"

CC=clang CXX=clang++ \
cmake -S . -B build \
  -G Ninja \
  -DOpenMP_ROOT="$MACOS_LIBOMP_PREFIX" \
  -DOpenMP_C_FLAGS="-Xpreprocessor -fopenmp" \
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp" \
  -DOpenMP_C_LIB_NAMES="omp" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY="$MACOS_LIBOMP_PREFIX/lib/libomp.dylib" \
  -Dspring_optimize_for_native=OFF \
  -Dspring_optimize_for_portability=ON

cmake --build build --parallel
```

Optional analysis configuration used in CI:

```bash
export HOMEBREW_LLVM_BIN="$(brew --prefix llvm)/bin"
export PATH="$HOMEBREW_LLVM_BIN:$PATH"
```

### Windows

CI uses MSYS2 UCRT64. Installing the same environment locally is the easiest way to match the supported build.

Install MSYS2 from https://www.msys2.org/ and then open an `MSYS2 UCRT64` shell.

Install the build requirements:

```bash
pacman -Syu
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-nasm \
  mingw-w64-ucrt-x86_64-ninja
```

Configure and build:

```bash
CC=gcc CXX=g++ \
cmake -S . -B build \
  -G Ninja \
  -Dspring_optimize_for_native=OFF \
  -Dspring_optimize_for_portability=ON

cmake --build build --parallel
```

Optional analysis tools used in CI:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-clang-tools-extra \
  mingw-w64-ucrt-x86_64-cppcheck \
  mingw-w64-ucrt-x86_64-python
```

## Running SPRING

The built executable is:

```bash
build/spring
```

If installed with conda, use:

```bash
spring
```

Current command-line help:

```text
Allowed options:
  -h [ --help ]                   produce help message
  -c [ --compress ]               compress
  -d [ --decompress ]             decompress
  --decompress-range arg          --decompress-range start end
                                  (optional) decompress only reads (or read
                                  pairs for PE datasets) from start to end
                                  (both inclusive) (1 <= start <= end <=
                                  num_reads (or num_read_pairs for PE)). If -r
                                  was specified during compression, the range
                                  of reads does not correspond to the original
                                  order of reads in the FASTQ file.
  -i [ --input-file ] arg         input file name (two files for paired end)
  -o [ --output-file ] arg        output file name (for paired end
                                  decompression, if only one file is specified,
                                  two output files will be created by suffixing
                                  .1 and .2.)
  -w [ --working-dir ] arg (=.)   directory to create temporary files (default
                                  current directory)
  -t [ --num-threads ] arg        number of threads (default:
                                  min(max(1, hw_threads - 1), 16))
  --memory-cap-gb arg (=0)        approximate memory budget in GB; reduces
                                  effective thread count using about 1 GB per
                                  worker thread (0 disables)
  -r [ --allow-read-reordering ]  do not retain read order during compression
                                  (paired reads still remain paired)
  --no-quality                    do not retain quality values during
                                  compression
  --no-ids                        do not retain read identifiers during
                                  compression
  -q [ --quality-opts ] arg       quality mode: possible modes are
                                  1. -q lossless (default)
                                  2. -q qvz qv_ratio (QVZ lossy compression,
                                  parameter qv_ratio roughly corresponds to
                                  bits used per quality value)
                                  3. -q ill_bin (Illumina 8-level binning)
                                  4. -q binary thr high low (binary (2-level)
                                  thresholding, quality binned to high if >=
                                  thr and to low if < thr)
  -l [ --long ]                   Use for compression of arbitrarily long read
                                  lengths. Can also provide better compression
                                  for reads with significant number of indels.
                                  -r disabled in this mode. For Illumina short
                                  reads, compression is better without -l flag.
  --gzip-level arg (=6)           gzip level (0-9) to use during decompression
                                  when the output path ends in .gz (default: 6)
```

SPRING archives are tar files containing the internal compressed streams, though using a `.spring` extension is recommended.

`--memory-cap-gb` is a conservative safety knob for machines with many cores and limited RAM. It does not hard-limit total allocation. Instead, it reduces the effective worker-thread count using an approximate budget of about 1 GB per worker thread.

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

For memory and CPU performance numbers, see the paper and supplementary material. SPRING also uses temporary disk space during compression.

In short-read mode, when qualities and identifiers are retained:

- default lossless mode typically uses temporary disk space around 10% to 30% of the original uncompressed input
- `-r` mode can push temporary disk usage much higher, often around 70% to 80% of the original file size

These figures are approximate and include the space needed for the final compressed output.

## Example Usage

Compress paired-end FASTQ losslessly:

```bash
./spring -c -i file_1.fastq file_2.fastq -o file.spring
```

Compress gzipped paired-end FASTQ losslessly:

```bash
./spring -c -i file_1.fastq.gz file_2.fastq.gz -o file.spring
```

Compress with 16 threads:

```bash
./spring -c -i file_1.fastq file_2.fastq -o file.spring -t 16
```

Compress with Illumina binning and no stored identifiers:

```bash
./spring -c -i file_1.fastq file_2.fastq -r --no-ids -q ill_bin -o file.spring
```

Compress with binary-thresholded qualities:

```bash
./spring -c -i file_1.fastq file_2.fastq -r --no-ids -q binary 20 40 6 -o file.spring
```

Compress with QVZ quantization:

```bash
./spring -c -i file_1.fastq file_2.fastq -r --no-ids -q qvz 1.0 -o file.spring
```

Compress reads and identifiers only:

```bash
./spring -c -i file_1.fastq file_2.fastq --no-quality -o file.spring
```

Compress single-end long reads:

```bash
./spring -c -l -i file.fastq -o file.spring
```

Compress single-end data without preserving order:

```bash
./spring -c -i file.fastq -r -o file.spring
```

Decompress single-end data:

```bash
./spring -d -i file.spring -o file.fastq
```

Decompress a range of reads:

```bash
./spring -d -i file.spring -o file.fastq --decompress-range 400 1000000
```

Decompress paired-end data to suffixed outputs:

```bash
./spring -d -i file.spring -o file.fastq
```

Decompress paired-end data to explicit outputs:

```bash
./spring -d -i file.spring -o file_1.fastq file_2.fastq
```

Decompress paired-end data directly to gzip outputs:

```bash
./spring -d -i file.spring -o file_1.fastq.gz file_2.fastq.gz
```

Decompress a paired-end read range:

```bash
./spring -d -i file.spring -o file_1.fastq file_2.fastq --decompress-range 4000000 8000000
```

Compress paired-end FASTA losslessly:

```bash
./spring -c -i file_1.fasta file_2.fasta -o file.spring
```

Decompress paired-end FASTA:

```bash
./spring -d -i file.spring -o file_1.fasta file_2.fasta
```

## Related

- QVZ: https://github.com/mikelhernaez/qvz/
- [Bioinformatics publication](https://academic.oup.com/bioinformatics/advance-article/doi/10.1093/bioinformatics/bty1015/5232998?guestAccessKey=266a1378-4684-4f04-bb99-6febdf9d1fb9)
- Specialized tool for nanopore long reads: https://github.com/qm2/NanoSpring
