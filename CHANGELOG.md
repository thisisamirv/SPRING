<!-- markdownlint-disable MD024 -->
# Changelog

## V1.0.0-beta

### Added

* Implemented **Archive Integrity Auditing**: Added record-level CRC32 digests (Sequence, ID, Quality) to archive metadata to guarantee 100% data fidelity for lossless archives.
* Integrated mandatory integrity verification for all standard decompression operations.
* Added the `-a, --audit` optional flag to `spring2` (for post-compression verification) and `spring2-preview` (for high-speed dry-run auditing).
* Added `tests/integrity_test.cpp` to validate end-to-end data fidelity and corruption detection.
* Expanded the `SpringReader` API with `get_digests()` to enable programmatic integrity verification for library consumers.
* Implemented a public library-style **Streaming Decompression API** (`SpringReader`) for SPRING2 archives, enabling external tools to consume genomic records programmatically without intermediate file I/O.
* Added the `DecompressionSink` abstract interface, allowing the decompression engine to push reconstructed records to arbitrary consumers (files, memory buffers, or network streams).
* Integrated a high-performance **Asynchronous Producer-Consumer model** within `SpringReader` that leverages background pre-fetching to maintain maximum throughout during streaming.
* Added robust stream error checking in `src/decompress.cpp` to handle corrupt or truncated archives during position and orientation decoding.
* Replaced system `tar` subprocess calls with the native `libarchive` library, removing a major runtime dependency and improving cross-platform compatibility.
* Added `src/scoped_temp_file.h` providing `ScopedTempFile` RAII helper to safely remove temporary files in a noexcept destructor; replaces ad-hoc temp-file handling to reduce races and ensure deterministic cleanup.
* Added the `-V, --version` flag to both `spring2` and `spring2-preview` tools for reporting the application version.
* Integrated the **doctest** unit testing framework for granular logic verification.
* Added a suite of unit tests in `tests/unit_tests.cpp` covering core utility functions (sequence manipulation, parsing, string helpers).
* Integrated both unit and smoke tests into the unified `ctest` workflow.
* Implemented a native Windows backend for `MmapView` in `src/raii.h`, enabling high-performance memory-mapped I/O on Windows.
* Added automatic compiler-cache launcher support in CMake (`SPRING_ENABLE_COMPILER_CACHE`, default ON).
* Added automatic fast-linker selection in CMake (`SPRING_ENABLE_FAST_LINKER`, default ON), preferring `mold` on Linux and falling back to `lld` where supported.

### Changed

* Optimized `reference_sequence_store::find_chunk_index` in `src/decompress.cpp` by replacing the linear scan with a binary search on chunk start offsets.
* Optimized parallel gzip FASTQ writing in `src/util.cpp` by implementing thread-local reusable buffers and a persistent `libdeflate_compressor` cache to reduce heap allocator pressure.
* Optimized `merge_paired_n_reads` in `src/preprocess.cpp` to use buffered I/O, reducing the number of disk operations when merging N-read positions.
* Modernized the monolithic `compression_params` struct by decomposing it into nested, cohesive component structures (`EncodingConfig`, `QualityConfig`, `GzipMetadata`, `ReadMetadata`). This improves type safety and clarifies field ownership across the compression and decompression pipelines.
* Refactored `src/encoder.h` into a thin interface header, moving template implementations to `src/encoder_impl.h` to improve compilation performance and modularity.
* Added `src/raii.h` with RAII helpers (`OmpLock`, `OmpLockGuard`, `MmapView`); migrated `src/decompress.cpp`, `src/reorder_impl.h`, and `src/encoder_impl.h` to use RAII for `mmap` and OpenMP locks, and adopted RAII containers (`std::vector`, `std::unique_ptr`, `std::array`) across preprocessing and hot-path modules (e.g., `src/preprocess.cpp`, buffer and stream ownership in `src/decompress.cpp`) to replace manual `new[]/delete[]`, manage gzip streams, and improve resource safety.
* Hardened filesystem cleanup and temporary-file handling: added non-throwing `safe_remove_file()` and `safe_rename_file()` in `src/util.h`/`src/util.cpp` and replaced unchecked `remove()`/`rename()` call sites across `src/` with these helpers; added `src/scoped_temp_file.h` for noexcept temporary-file cleanup and made mapped-file RAII (`MmapView`) destructor `noexcept` to ensure safe unmapping during stack unwinding.
* Replaced hot-path raw arrays with `std::vector` or `std::unique_ptr<T[]>` starting in `src/reorder_impl.h` and `src/encoder_impl.h` to improve memory safety and eliminate manual `new[]/delete[]` usage.
* Replaced manual `char**` decoded noise table in `src/decompress.cpp` with `std::array<std::array<char,128>,128>`, removing manual `new[]/delete[]` and ensuring RAII-managed lifetime.
* Refactored `src/reorder.h` and the corresponding reordering implementation in `src/reorder_impl.h` to reduce template bloat and improve compilation speed.
* Modernized memory management in `bbhashdict` (the core read dictionary) by replacing manual `new[]/delete[]` with `std::unique_ptr` and `std::make_unique`.
* Refactored the `main.cpp` entry point to use a centralized RAII `SpringContext` class for managing temporary directories and resource cleanup, significantly reducing global state and improving signal-safety.
* Improved development documentation in `DEVELOPMENT.md` with a new section on unit testing.
* Refactored the monolithic `util.h` and `util.cpp` into a modular, domain-specific architecture:
  * `src/io_utils.h/cpp`: Specialized for C-SiT ID compression, QVZ quantization logic, and robust binary I/O.
  * `src/dna_utils.h/cpp`: Centralized DNA sequence manipulation (reverse complement, bit-packing, N-read encoding).
  * `src/parse_utils.h/cpp`: Handle ID pattern matching, FASTQ block parsing, and numeric string conversions.
  * `src/fs_utils.h/cpp`: Granular filesystem helpers and RAII file management.
  * `src/params.h/cpp`: Serialization of compression parameters and metadata structures to resolve circular dependencies.
* Refactored the core decompression engine in `src/decompress.cpp` and `src/decompress.h` into a stateful, sink-based architecture to support the new streaming reader while maintaining bit-perfect CLI backward compatibility.
* Upgraded the CLI verbosity system from a binary toggle to explicit log levels: default quiet mode keeps the progress bar, `--verbose`/`--verbose info` enables informational logs, and `--verbose debug` enables both informational and detailed debug diagnostics.
* Pruned indexed_bzip2 even more.
* Made windows build process guide easier and more straightforward.
* Expanded CMake runtime portability packaging: improved Windows GCC/OpenMP runtime DLL resolution and bundling for `spring2`, `spring2-preview`, and `rapidgzip`; added install-time RPATH configuration and OpenMP runtime library bundling support for macOS (`@loader_path/../lib`) and Linux (`$ORIGIN/../lib`).
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
