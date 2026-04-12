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

### Smoke Tests

A basic round-trip validation script is provided:

```bash
./tests/smoke_test.sh
```

## Release Pipeline

The project uses GitHub Actions for automated multi-architecture releases.

- **Runners**: Uses `ubuntu-latest` (x86_64) and `ubuntu-24.04-arm64` (native ARM silicon).
- **Environment**: Builds are performed inside **AlmaLinux 8** Docker containers to pin the GLIBC baseline to **2.28**, ensuring maximum portability across enterprise distributions (RHEL 8+, Ubuntu 20.04+, etc.).
- **Artifacts**: The pipeline produces portable AppImages for Linux, universal Mach-O binaries for macOS, and native executables for Windows.
