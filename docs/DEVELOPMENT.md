# Maintaining and Developing SPRING2

This guide is for developers who want to contribute to SPRING2 or maintain the build system and delivery pipelines.

## Project Structure

- `src/`: Core C++20 source files and headers.
- `vendor/`: Bundled third-party dependencies as `.tar.xz` archives.
- `docs/`: Project documentation.
- `dev/`: Helper scripts for linting and development-time checks.
- `tests/`: Integration and smoke tests.

## Build System Architecture

SPRING2 uses a modular CMake build system. Each vendor dependency in the `vendor/` directory is self-contained:

- Dependencies are extracted on-the-fly during CMake configuration.
- Each major dependency (e.g., `libbsc`, `qvz`, `cloudflare_zlib`) has its own internal `CMakeLists.txt` managing its build logic.
- All internal libraries are built with `POSITION_INDEPENDENT_CODE ON` to ensure compatibility with enterprise Linux (PIE) requirements.

### Adding or Updating a Dependency

1. Extract the existing tarball in `vendor/`.
2. Make your changes or update the source.
3. If adding a new `CMakeLists.txt`, ensure it defines the necessary targets and sets PIC.
4. Re-archive the folder: `tar -cJf my-lib.tar.xz my-lib/`.

## Quality Control

### Linting

We use `clang-format` and several custom lint scripts. You can run the convenience wrapper:

```bash
./dev/lint.sh
```

### Compiler Cache (Faster Rebuilds)

For faster incremental development builds, SPRING2 supports compiler cache launchers during CMake configure:

- Windows: `sccache`
- Linux/macOS: `ccache` (falls back to `sccache` if `ccache` is unavailable)

This behavior is enabled by default with `-DSPRING_ENABLE_COMPILER_CACHE=ON`.

Install suggestions:

- Linux (Debian/Ubuntu): `sudo apt-get install -y ccache`
- macOS (Homebrew): `brew install ccache`
- Windows (winget): `winget install --id Mozilla.sccache -e`

Disable compiler cache explicitly (if needed):

```bash
cmake -S . -B build -G Ninja -DSPRING_ENABLE_COMPILER_CACHE=OFF
```

Check cache stats:

- `ccache -s` (Linux/macOS)
- `sccache --show-stats` (Windows, or any platform using `sccache`)

### Faster Linker (Where Supported)

SPRING2 can also select a faster linker during CMake configure, enabled by default with `-DSPRING_ENABLE_FAST_LINKER=ON`.

Selection order:

- Linux: tries `mold` first (`-fuse-ld=mold`), then `lld` (`-fuse-ld=lld`)
- macOS/Windows (GNU/Clang toolchains): tries `lld` when supported

Install suggestions:

- Linux (Debian/Ubuntu): `sudo apt-get install -y mold lld`
- macOS (Homebrew): `brew install lld`
- Windows (MSYS2/MinGW-w64 UCRT): `pacman -S --needed mingw-w64-ucrt-x86_64-lld`

If no compatible fast linker is available, the build falls back to the platform default linker automatically.

Disable this behavior explicitly (if needed):

```bash
cmake -S . -B build -G Ninja -DSPRING_ENABLE_FAST_LINKER=OFF
```

### Precompiled Headers (PCH)

SPRING2 uses precompiled headers to accelerate incremental rebuilds, enabled by default with `-DSPRING_ENABLE_PRECOMPILED_HEADERS=ON`.

The precompiled header (`src/pch.h`) includes stable, frequently-used headers:

- Standard library containers (`<vector>`, `<string>`, `<array>`)
- I/O utilities (`<iostream>`, `<fstream>`, `<iomanip>`)
- File system and concurrency (`<filesystem>`, `<mutex>`, `<thread>`, `<atomic>`)
- OpenMP (`<omp.h>`)

When enabled, CMake compiles this header once and caches it, significantly reducing compile time on subsequent rebuilds. The improvement is especially noticeable after touching a few files or after clean configuration, where PCH cache is reused across translation units.

Disable precompiled headers explicitly (if needed):

```bash
cmake -S . -B build -G Ninja -DSPRING_ENABLE_PRECOMPILED_HEADERS=OFF
```

### Unit and Integration Tests

We use the **doctest** framework for both granular unit testing and high-level integration testing. You can build and run all tests using standard CMake/CTest workflows:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

- **Unit Tests**: `tests/unit_tests.cpp` (Utility functions, parsing, DNA manipulation).
- **Streaming API Integration Tests**: `tests/reader_test.cpp` (Verifies end-to-end archive streaming).
- **Smoke Tests**: `tests/smoke_test.sh` (CLI behavioral validation).

### Diagnostic Key Legend

Many debug logs use a normalized key schema for fast triage:

- `block_id`: Pipeline stage or thread/block identifier that emitted the log.
- `path`: File path or logical input source being checked.
- `expected_bytes`: Expected size/count/bound for the check.
- `actual_bytes`: Observed size/count/value at runtime.
- `index`: Loop position, record number, or block-local offset where the check failed.

Interpretation tips:

- `expected_bytes=1, actual_bytes=0`: Usually open/availability failure (resource not usable).
- `actual_bytes < expected_bytes`: Typical short read/truncation signal.
- `actual_bytes > expected_bytes`: Often out-of-range metadata/value for bound checks.
- `index` pinpoints the offending element; combine with `block_id` to locate the stage.

## Release Pipeline

The project uses GitHub Actions for automated multi-architecture releases.

- **Runners**: Uses `ubuntu-latest` (x86_64) and `ubuntu-24.04-arm64` (native ARM silicon).
- **Environment**: Builds are performed inside **AlmaLinux 8** Docker containers to pin the GLIBC baseline to **2.28**, ensuring maximum portability across enterprise distributions (RHEL 8+, Ubuntu 20.04+, etc.).
- **Artifacts**: The pipeline produces portable AppImages for Linux, universal Mach-O binaries for macOS, and native executables for Windows.
