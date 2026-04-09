<!-- markdownlint-disable MD033 -->
# Building SPRING2

This document provides instructions for building SPRING2 from source on various platforms.

## Platform Support

The current CI workflow validates SPRING on:

- Linux
- macOS
- Windows (native MinGW-w64)

The build system currently requires:

- A C++20-capable compiler
- CMake 4.2 or newer
- Ninja
- NASM
- OpenMP

## Getting the Source

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

### Windows (native MinGW-w64)

You can build using a standalone MinGW-w64 (UCRT) distribution and native CMake/Ninja.

What to install (exact recommendations):

- MinGW-w64 UCRT64 (standalone): get a winlibs MinGW build
  <https://winlibs.com/> — download the latest "ucrt64" archive and extract it to C:\Program Files\mingw-w64\ucrt64\
- CMake (>= 4.2): Windows installer from Kitware
  <https://cmake.org/download/>
- Ninja: download single `ninja.exe` binary and extract it to C:\Program Files\Ninja\
  <https://github.com/ninja-build/ninja/releases>
- NASM (assembler): Windows installer
  <https://www.nasm.us/>
- Gzip: required for gzipped FASTQ support. You can use the one included in Git for Windows or a standalone binary.
  Ensure `gzip.exe` is in your PATH.

Arrange PATH (temporary for current cmd session). Open a new `PowerShell` and run (adjust paths to where you installed/extracted):

```powershell
$env:PATH="C:\Program Files\mingw-w64\ucrt64\bin;C:\Program Files\CMake\bin;C:\Program Files\Ninja\ninja.exe;$env:PATH"
$env:CC="gcc"
$env:CXX="g++"
```

Configure & build (native cmd) from the repo root:

```powershell
cmake -S . -B build -G Ninja -DSPRING_STATIC_RUNTIMES=OFF -Dspring_optimize_for_native=OFF -Dspring_optimize_for_portability=ON
cmake --build build --parallel
cmake --build build --target copy_runtime_dlls
```
