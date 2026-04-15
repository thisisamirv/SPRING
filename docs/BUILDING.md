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

By default, the build system uses `-Dspring_optimize_for_native=OFF -Dspring_optimize_for_portability=ON` to produce binaries that are compatible across a wide range of architectures (requiring only SSE4.1 on x86_64). For a build specifically tuned to your local machine, you can pass `-Dspring_optimize_for_native=ON -Dspring_optimize_for_portability=OFF`.

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
cmake -S . -B build -G Ninja
cmake --build build --parallel
cmake --install build --prefix spring2
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
CC=clang CXX=clang++ cmake -S . -B build -G Ninja
cmake --build build --parallel
cmake --install build --prefix spring2
```

### Windows (native MinGW-w64)

You can build using a standalone MinGW-w64 (UCRT) distribution and native CMake/Ninja:

```powershell
winget install --id BrechtSanders.WinLibs.MCF.UCRT -e
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
winget install --id NASM.NASM -e
winget install --id Git.Git -e
```

Arrange PATH in `PowerShell`:

```powershell
$env:CC="gcc"
$env:CXX="g++"
```

Configure & build from the repo root:

```powershell
cmake -S . -B build -G Ninja
cmake --build build --parallel
cmake --install build --prefix spring2
```
