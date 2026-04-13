<!-- markdownlint-disable MD024 -->
# Changelog

## V1.0.0-beta.1

### Added

* Added robust stream error checking in `src/decompress.cpp` to handle corrupt or truncated archives during position and orientation decoding.
* Replaced system `tar` subprocess calls with the native `libarchive` library, removing a major runtime dependency and improving cross-platform compatibility.

### Changed

* Optimized `reference_sequence_store::find_chunk_index` in `src/decompress.cpp` by replacing the linear scan with a binary search on chunk start offsets.
* Optimized parallel gzip FASTQ writing in `src/util.cpp` by implementing thread-local reusable buffers and a persistent `libdeflate_compressor` cache to reduce heap allocator pressure.
* Optimized `merge_paired_n_reads` in `src/preprocess.cpp` to use buffered I/O, reducing the number of disk operations when merging N-read positions.
* Modernized the monolithic `compression_params` struct by decomposing it into nested, cohesive component structures (`EncodingConfig`, `QualityConfig`, `GzipMetadata`, `ReadMetadata`). This improves type safety and clarifies field ownership across the compression and decompression pipelines.
* Refactored `src/encoder.h` into a thin interface header, moving template implementations to `src/encoder_impl.h` to improve compilation performance and modularity.
* Added `src/raii.h` with RAII helpers (`OmpLock`, `OmpLockGuard`, `MmapView`); migrated `src/decompress.cpp`, `src/reorder_impl.h`, and `src/encoder_impl.h` to use RAII for `mmap` and OpenMP locks, fixing teardown bugs and preventing resource leaks.
* Replaced hot-path raw arrays with `std::vector` or `std::unique_ptr<T[]>` starting in `src/reorder_impl.h` and `src/encoder_impl.h` to improve memory safety and eliminate manual `new[]/delete[]` usage.
* Refactored `src/reorder.h` and the corresponding reordering implementation in `src/reorder_impl.h` to reduce template bloat and improve compilation speed.
* Consolidated all third-party dependency licenses into the central root `LICENSE` file for improved legal compliance and audit-readiness.

### Fixed

* Removed debug `[GZIP-DIAG]` logs from the compression pipeline that were firing even in non-verbose mode.
* Consolidated `parse_int_or_throw`, `parse_double_or_throw`, and `parse_uint64_or_throw` into `src/util.h` and `src/util.cpp` to remove duplication and potential ODR hazards.
* Consolidated `has_suffix` into `src/util.h` and `src/util.cpp`, removing duplicate definitions in `src/spring.cpp` and `src/decompress.cpp`.
* Removed a redundant duplicate call to `generatemasks` in the encoder initialization.
* Fixed an issue in `src/spring.cpp` where missing archive metadata could lead to silent decompression failures by adding validation for inferred output paths.
* Removed redundant dead code in decompression IO resolution logic.

## V1.0.0-alpha

### Added

* Formally rebranded the software to **SPRING2**.
* Renamed the project and executable binary to `spring2`.
* Added a robust CMake `install` target to create a clean, portable distribution footprint (e.g., in a `dist/` or `spring2/` directory) containing only final binaries and required runtime libraries.
* Added benchmark scripts under `benchmark/` for lossless round-trip runs, comparison runs, and larger manual benchmarking workflows.
* Added round-trip integrity verification to the lossless benchmark flow, including checksum reporting when hashing tools are available.
* Added automatic support for gzipped compression inputs by staging `.gz` inputs into the temporary working directory before normal compression.
* Added automatic short-read versus long-read mode detection by pre-scanning input sequence lengths before compression.
* Added the `--memory` (`-m`) CLI option to conservatively reduce effective worker count on memory-constrained systems.
* Added a unified `-s, --strip [ioq]` CLI flag to discard identifiers (`i`), order (`o`), and quality scores (`q`), replacing independent flags.
* Added vendored `libdeflate` for fast whole-buffer DEFLATE, zlib, and gzip workloads used by the current build.
* Added vendored `rapidgzip` support for gzipped compression inputs through the pruned `indexed_bzip2` payload.
* Added a dedicated `dev/` tooling directory for repository maintenance, including linting, cppcheck, cbindgen validation, Valgrind smoke checks, shared helpers, and suppressions.
* Added configure-time `.clangd` generation so editor diagnostics inherit the active compiler's include paths and OpenMP configuration.
* Added extensive documentations.
* Added storage of the original input filenames and detailed gzip metadata (BGZF block size, header flags, MTIME, OS, and internal filename) within the compressed archive metadata.
* Added the `spring2-preview` utility for inspecting archive metadata, read counts, settings, and detailed compression ratios without full decompression.
* Added the `-n, --note` flag to store custom text notes within the archive.
* Added the `-u, --unzip` flag to force uncompressed output during decompression, even if the original input was gzipped.
* Added a specialized `-v, --verbose` flag to toggle between a real-time progress bar (default) and detailed informational logging.

### Changed

* Replaced the unmaintained legacy `id_comp` module with a natively integrated **Columnar Specialized Identifier Coder (C-SiT)** backed by Zstd (Level 22 max-compression). C-SiT dynamically parses FASTQ machine headers into dedicated columnar streams. For tile coordinates, it leverages an advanced auto-detecting Byte-Shuffled Delta Encoder that shrinks numeric identifiers into overlapping low-entropy sequences.
* Improved benchmark reporting so compression and decompression runs report elapsed time, CPU time, average core usage, and peak RSS when supported by the host environment.
* Changed the default thread selection logic to `min(max(1, hw_threads - 1), 16)`.
* Changed decompression output handling so output paths ending in `.gz` automatically produce gzipped FASTQ output.
* Replaced the `--gzip-level` option with a unified `-l, --level` flag (range 1–9, default 6). Values are passed to gzip for compressed output and scaled to Zstd (1–22) for internal streams.
* Renamed several core CLI flags for clarity and standard usage: `--num-threads` to `--threads` (`-t`), `--input-file` to `--input` (`-i`), `--output-file` to `--output` (`-o`), `--working-dir` to `--tmp-dir` (`-w`), and `--quality-opts` to `--qmod` (`-q`).
* Transitioned the recommended archive file extension from `.spring` to `.sp`.
* Removed the obsolete `--gzipped-fastq`, `--fasta-input`, and manual `-l` (long-read) flags.
* Repackaged `indexed_bzip2` into a smaller Spring-specific archive payload that retains only the pieces needed for the current gzip workflow.
* Removed the final Boost dependency from the build and runtime path by replacing the remaining Boost-based gzip and mapped-file usage with local implementations.
* Upgraded the project toolchain baseline to C++20 and CMake 4.2.
* Refreshed the vendored dependency set used by the current tree, including `libbsc`, Cloudflare zlib, `libdeflate`, `qvz`, and the pruned `indexed_bzip2` payload.
* Replaced the unmaintained BBHash with a patched version of PTHash (making it more compatible with windows) for the hash table implementation.
* Made vendor extraction idempotent so repeated configure runs only re-extract archives when their content hash changes.
* Standardized formatting with the repository `.clang-format` configuration.
* Renamed several core source files to clearer role-based names, including `bitset_dictionary`, `template_dispatch`, `paired_end_order`, `reordered_quality_id`, and `reordered_streams`.
* Reorganized the internal implementation to reduce duplicated scaffolding across compression, decompression, preprocessing, template dispatch, and other core subsystems while preserving behavior.
* Accelerated the **QVZ lossy compression mode** by up to 1000x through a series of algorithmic optimizations:
  * Refactored the distortion matrix calculation from $O(N^2)$ to $O(N)$ using prefix sums.
  * Integrated a binary search for optimal quantization states and added a short-circuit path for ratio 1.0 targets.
  * Optimized the core probability propagation loops from $O(N^4)$ to $O(N^3)$ via invariant loop hoisting.

### Fixed

* Removed the previous Unix-only build assumption and enabled the modernized build and CI flow on Windows through a native MinGW-w64 toolchain.
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
* Fixed a memory corruption hazard in the `qvz` module by standardizing `ALPHABET_SIZE` at 128 to accommodate contemporary high-range Phred scores.
* Fixed a scope error in the `qvz` custom distortion matrix generator.
