# Changelog

## [Unreleased]

### Added

* Added benchmark scripts under `benchmark/` for lossless round-trip runs, comparison runs, and larger manual benchmarking workflows.
* Added round-trip integrity verification to the lossless benchmark flow, including checksum reporting when hashing tools are available.
* Added automatic support for gzipped compression inputs by staging `.gz` inputs into the temporary working directory before normal compression.
* Added automatic FASTA-versus-FASTQ input detection during compression.
* Added the `--memory-cap-gb` CLI option to conservatively reduce effective worker count on memory-constrained systems.
* Added vendored `libdeflate` for fast whole-buffer DEFLATE, zlib, and gzip workloads used by the current build.
* Added vendored `rapidgzip` support for gzipped compression inputs through the pruned `indexed_bzip2` payload.
* Added a dedicated `dev/` tooling directory for repository maintenance, including linting, cppcheck, cbindgen validation, Valgrind smoke checks, shared helpers, and suppressions.
* Added configure-time `.clangd` generation so editor diagnostics inherit the active compiler's include paths and OpenMP configuration.

### Changed

* Improved benchmark reporting so compression and decompression runs report elapsed time, CPU time, average core usage, and peak RSS when supported by the host environment.
* Changed the default thread selection logic to `min(max(1, hw_threads - 1), 16)` instead of a fixed default.
* Changed decompression output handling so output paths ending in `.gz` automatically produce gzipped FASTQ output, while preserving `--gzip-level`.
* Removed the obsolete `--gzipped-fastq` and `--fasta-input` flags because the corresponding behaviors are now inferred automatically.
* Repackaged `indexed_bzip2` into a smaller Spring-specific archive payload that retains only the pieces needed for the current gzip workflow.
* Removed the final Boost dependency from the build and runtime path by replacing the remaining Boost-based gzip and mapped-file usage with local implementations.
* Upgraded the project toolchain baseline to C++20 and CMake 4.2.
* Refreshed the vendored dependency set used by the current tree, including `libbsc`, Cloudflare zlib, BBHash, `libdeflate`, `id_compression`, `qvz`, and the pruned `indexed_bzip2` payload.
* Made vendor extraction idempotent so repeated configure runs only re-extract archives when their content hash changes.
* Standardized formatting with the repository `.clang-format` configuration.
* Renamed several core source files to clearer role-based names, including `bitset_dictionary`, `template_dispatch`, `paired_end_order`, `reordered_quality_id`, and `reordered_streams`.
* Reorganized the internal implementation to reduce duplicated scaffolding across compression, decompression, preprocessing, template dispatch, and other core subsystems while preserving behavior.

### Fixed

* Removed the previous Unix-only build assumption and enabled the modernized build and CI flow on Windows through the MSYS2 UCRT64 toolchain.
* Fixed empty-reference-chunk decompression failures by switching the decoded chunk path to the local mapping wrapper.
* Reduced decompression peak memory usage by avoiding reconstruction of the full decoded reference in one large in-memory string.
* Reduced decompression write overhead by switching packed-sequence decode output to buffered block writes.
* Reduced compression-side sequence-packing overhead by eliminating the extra prepass and moving to buffered reads and writes.
* Reduced quality and identifier staging overhead by partitioning in a single pass and reloading batches more efficiently.
* Improved compiler portability by adding missing standard-library includes, replacing non-standard integer aliases with standard types, and fixing newer GCC compatibility issues in both first-party and vendored code.
* Fixed thread-count validation and related allocation hazards in CLI and internal helper paths.
* Fixed binary quality range validation for `-q binary` to avoid out-of-range table access and associated compiler warnings.
* Cleaned up lint and cppcheck findings across the current codebase, including missing initialization, binary I/O casts, signed-shift portability, and exception-safety issues.
* Tightened targeted lint-path handling and Python-file validation in the developer tooling.
* Stabilized the Valgrind smoke workflow by focusing failures on actionable leak classes and suppressing known OpenMP and libc startup noise.
