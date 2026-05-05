<!-- markdownlint-disable MD033 -->

# Building SPRING2

This document provides instructions for building SPRING2 from source on various platforms.

## Platform Support

The current CI workflow validates SPRING2 on:

- Linux
- macOS
- Windows (native MinGW-w64)

The CMake build supports these compiler frontends:

- GCC
- Clang / Apple Clang
- MSVC
- IntelLLVM

The build system currently requires:

- A C++20-capable compiler
- CMake 4.2 or newer
- OpenMP

Vendored copies of Ninja and NASM are used automatically from `tools/host/ninja/` and `tools/host/nasm/` when available, so you do not need separate system installations for those tools on the supported host platforms.

## Getting the Source

Clone the repository:

```bash
git clone https://github.com/thisisamirv/SPRING2.git
cd SPRING2
```

By default, the build system uses `-Dspring_optimize_for_native=OFF -Dspring_optimize_for_portability=ON` to produce binaries that are compatible across a wide range of architectures (requiring only SSE4.1 on x86_64). For a build specifically tuned to your local machine, you can pass `-Dspring_optimize_for_native=ON -Dspring_optimize_for_portability=OFF`.

### Linux

Install the build requirements:

```bash
sudo apt-get update
sudo apt-get install -y build-essential libomp-dev python3 python3-pip
python3 -m pip install --upgrade --user pip
python3 -m pip install --user "cmake==4.2.0"
```

If your Python user bin directory is not already on `PATH`, add it before running CMake:

```bash
export PATH="$(python3 -m site --user-base)/bin:$PATH"
```

Configure and build:

```bash
cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix dist
```

If you want the compilation database available immediately for editor tooling, either build once as shown above or copy `out/build/compile_commands.json` into `out/clangd/compile_commands.json`.

### macOS

Install Xcode Command Line Tools first if needed:

```bash
xcode-select --install
```

Install Homebrew packages:

```bash
brew update
brew install libomp python llvm cppcheck
python3 -m pip install --upgrade --user pip
python3 -m pip install --user "cmake==4.2.0"
```

If your Python user bin directory is not already on `PATH`, add it before running CMake:

```bash
export PATH="$(python3 -m site --user-base)/bin:$PATH"
```

Configure and build with Apple Clang and Homebrew `libomp`:

```bash
MACOS_LIBOMP_PREFIX="$(brew --prefix libomp)"
CC=clang CXX=clang++ cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix dist
```

### Windows (native MinGW-w64)

You can build using a standalone MinGW-w64 (UCRT) distribution and native CMake:

```powershell
winget install --id BrechtSanders.WinLibs.MCF.UCRT -e
winget install --id Kitware.CMake -e
winget install --id Git.Git -e
```

Arrange PATH in `PowerShell`:

```powershell
$env:CC="gcc"
$env:CXX="g++"
```

Configure & build from the repo root:

```powershell
cmake -S . -B out/build -G Ninja
cmake --build out/build --parallel
cmake --install out/build --prefix dist
```

With the Ninja generator, CMake will use the vendored executable under `tools/host/ninja/` automatically. ISA-L will also use the vendored NASM under `tools/host/nasm/` automatically.

The resulting executable will be built under `out/build/`, and `cmake --install` will populate `dist/bin/`.

### Windows (Visual Studio / MSVC-compatible frontends)

You can also build with Visual Studio generators using either MSVC or ClangCL.

Configure with the default MSVC toolset:

```powershell
cmake -S . -B out/build-msvc -G "Visual Studio 18 2026" -A x64
cmake --build out/build-msvc --config Release --parallel
cmake --install out/build-msvc --config Release --prefix dist/msvc
```

Configure with ClangCL:

```powershell
cmake -S . -B out/build-clangcl -G "Visual Studio 18 2026" -A x64 -T ClangCL
cmake --build out/build-clangcl --config Release --parallel
cmake --install out/build-clangcl --config Release --prefix dist/clangcl
```

### Linux (IntelLLVM)

IntelLLVM can be used with the same Ninja-based flow on Linux when the oneAPI environment is active:

```bash
source /opt/intel/oneapi/setvars.sh
CC=icx CXX=icpx cmake -S . -B out/build-intel -G Ninja
cmake --build out/build-intel --parallel
cmake --install out/build-intel --prefix dist/intel
```
