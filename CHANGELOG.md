<!-- markdownlint-disable MD024 -->

# Changelog

## V1.0.0-rc.1

### Added

- Added grouped lane support for read-3 and index reads with `-R3/--R3`, `-I1/--I1`, and `-I2/--I2`: SPRING2 can now compress `R1/R2` together with a third read lane and/or index lanes and restore grouped outputs on decompression (`.R1`, `.R2`, `.R3`, `.I1`, `.I2`).
- Added automatic non-ACGTN sequence detection during input pre-scan: when extended IUPAC/RNA symbols are present, SPRING2 now switches to long-read mode to preserve sequence alphabet losslessly instead of using the short-read 3-bit path.
- Added the `-y/--assay` flag to specify and store the sequencing assay type in the archive metadata.
- Implemented **scoring-based assay detection system** (`--assay auto`) that combines evidence from multiple detection methods to classify sequencing chemistry and layout (RNA, ATAC, bisulfite/methylation, DNA, and single-cell variations). The detector samples the first 10,000 reads and aggregates confidence scores from: (1) base composition analysis with relaxed bisulfite thresholds (C/(C+T) and G/(G+A) ratios ≤0.10 for high confidence, ≤0.15 for moderate) to catch real-world conversion efficiencies, (2) FASTQ sequence signatures (Tn5 adapters for ATAC, poly-A/T tails for RNA with bisulfite false-positive suppression), and (3) reference k-mer alignment sketches to RNA exons, ATAC promoters, and genomic backbone regions. Single-cell layout detection uses multiple independent indicators including explicit index lanes (R3/I1/I2), CB:Z: and UMI tags in FASTQ headers, and read-length asymmetry patterns (short R1 with long R2). The final classification selects the assay type with the highest cumulative evidence score and provides detailed confidence reporting showing which signals contributed to the decision.
- Implemented **bisulfite-aware overlap-based compression for methylation assays**: 2-bit encoding and bitwise masking in the reordering dictionary allow the overlap-based encoder to match reads regardless of bisulfite conversion status (C/T and G/A), maximizing consensus contig length and compression ratios for methylated data while ensuring bit-perfect reconstruction.
- Added a small test FASTA reference (`ref_hg38_gencode49.fa`) under `reference/` and the corresponding metadata file for detecting assay type from input FASTQ files.
- Implemented **cell barcode prefix extraction for sc-RNA compression**: For single-cell RNA assays in preserve-order mode without external index lanes (I1/I2), the first `--cb-len` bases (default 16 bp) are extracted from R1 reads during preprocessing and stored in a separate compressed stream (`cb_prefix.dna`), allowing the overlap-based encoder to operate on the UMI-only portion of R1. This reduces R1 read length from 28bp (CB+UMI) to 12bp (UMI only), enabling more effective compression since the highly repetitive CB sequences are encoded separately from the random UMI data. CB prefixes are transparently restored during decompression. The optimization is lossless and automatically enabled for `--assay sc-rna` only when the cell barcode is embedded in R1 rather than provided through external index lanes. Grouped sc-RNA inputs that already carry CB data in I1/I2, such as `test_5`, do not use this path and therefore should not be expected to show a compression gain from CB-prefix extraction. Note: sc-bisulfite protocols vary in read structure (some place genomic sequence in R1, others use CB+UMI), so SPRING2 currently leaves CB extraction disabled for sc-bisulfite.
- Implemented **lossless terminal adapter stripping for ATAC / sc-ATAC compression**: For ATAC-family assays, SPRING2 now detects terminal Tn5/Nextera read-through sequence at the end of genomic reads, strips the matched suffix during preprocessing, and stores the removed bases in a synchronized side stream for exact restoration during decompression. The transform is enabled only when the detected adapter burden is high enough to be net beneficial, skips grouped index/read-3 lanes, and preserves bit-perfect reconstruction even for mixed-content datasets containing `N` reads.
- Implemented **index-aware grouped sc-RNA ID compression**: For grouped single-cell RNA archives with external `I1`/`I2` lanes, SPRING2 now detects the common Illumina-style layout where the trailing FASTQ identifier token already contains the index reads (`...:I1+I2`). In that case, the grouped index subarchive stores only the identifier prefix and reconstructs the trailing `I1`/`I2` token from the decoded index reads during decompression. This avoids compressing the same barcode token twice, remains fully lossless, and improves grouped sc-RNA compression when cell barcode / sample index bases are provided as explicit index lanes.
- Added SPRING2 version to the metadata of compressed files.
- Implemented **lossless poly-A/T tail stripping and restoration for RNA-seq assays**: Strips long terminal poly-A/T runs (>20bp) during preprocessing to improve compression ratios for transcriptomics data. The optimization is strictly lossless, storing stripped sequence bases and quality scores in a synchronized metadata stream that is reordered in tandem with reads. Restoration during decompression ensures identical reconstruction and passes all integrity audits. Activated automatically for high-confidence RNA assays or via `--assay rna`.
- Added regressions covering the shared 10,000-fragment startup sampler, late overlength escalation from sampled short-read input into long mode, and late CRLF metadata discovery during preprocessing retry.
- Added integration regressions covering grouped preview/audit corruption detection, grouped decompression output naming and explicit output lists, grouped `SpringReader` streaming, aliased grouped `R3` output formatting, mixed paired FASTQ plus-line and LF/CRLF round-trips, per-stream gzip reconstruction behavior, archive extraction path containment, preview and `SpringReader` rejection of truncated metadata, output-path collision rejection, grouped manifest validation, `SpringReader` digest retrieval, and graceful failure for corrupted long-read archives.
- Expanded compiler support.

### Changed

- Refactored source code into a more modular structure.
- Changed the compression CLI to require `-R1/--R1` for read 1 and accept optional `-R2/--R2` for paired-end mode; compression no longer uses `-i/--input`.
- Removed the obsolete working-directory CLI flag `-w/--tmp-dir` now that runtime working-directory use has been fully eliminated.
- Changed reordering algorithm to use deterministic matching.
- Replaced archived vendors with direct dependencies.
- Pruned vendored libs even further.
- Had vendored libs flattened for easier maintenance. This allows easier future pruning and specialized modifications.
- Merged preview into the main binary. spring2 now accepts `-p` or `--preview` and runs the old `spring2-preview` behavior, including `-a/--audit` in preview mode.
- Changed the compression, decompression, preview, audit, grouped-archive, and `SpringReader` pipelines to operate fully in memory, completely eliminating working-directory use and temporary working files across the runtime data path.
- Changed compression startup to reuse one shared 10,000-fragment sample for assay detection plus initial read-length, newline, and non-ACGTN analysis instead of running separate startup passes; sampled-long inputs still take the existing full prescan, while sampled-short inputs defer full validation to preprocessing.
- Changed sampled short-read startup detection so preprocessing now validates sampled max-read-length, CRLF, and non-ACGTN assumptions while streaming input, retries once from a clean restart when later data changes those properties, and escalates to the long-read path with a full prescan when late reads prove the sample was not representative.
- Vendored NASM and Ninja for easier development.

### Fixed

- Fixed decompression thread count mismatch bug in `src/decompress.cpp` where archives compressed with N threads could only be decompressed successfully with exactly N threads. The decompressor was incorrectly using the user-specified `-t` flag instead of the archive's encoded thread count metadata, causing incomplete block processing when fewer threads were used. Decompression now always uses the archive's encoding thread count internally (via `archive_encoding_thread_count`) regardless of the user's `-t` flag, ensuring archives can be decompressed on any hardware configuration.
- Fixed grouped preview, grouped `spring2 -p -a/--audit`, and grouped decompression handling so grouped bundles now execute the real audit path, preserve archive notes in preview output, and accept all five explicit output paths (`R1`, `R2`, `R3`, `I1`, `I2`).
- Fixed truncated-metadata handling by rejecting archives whose `cp.bin` parameter stream cannot be fully read, instead of continuing with partially parsed compression parameters.
- Fixed long-read decompression error handling in `src/decompress.cpp` by capturing exceptions inside the OpenMP worker region and rethrowing them on the main thread, preventing corrupted long-read blocks from terminating the whole process instead of returning a normal runtime error.
- Fixed archive assembly in `src/fs_utils.cpp` to fail loudly on `stat`, header, open, read, write, and finalize errors instead of silently producing truncated or incomplete tar archives.
- Fixed lossless paired FASTQ round-tripping for mixed formatting by storing quality-header style (`+` versus `+READ_ID`) and newline convention (LF versus CRLF) per stream in archive metadata, rather than collapsing both mates to one archive-wide setting; backward compatibility with older archives is preserved.
- Fixed grouped decompression when `R3` is stored as an alias of `R1` or `R2`: aliased outputs are now materialized through the normal decompression path so the requested target format is preserved instead of inheriting the source mate's on-disk representation.
- Fixed `SpringReader` so it can stream grouped bundle archives by resolving `bundle.meta`, loading the primary read member archive, and reading `cp.bin` plus decode streams from that nested archive instead of assuming a flat top-level layout.
- Fixed paired gzip output reconstruction so each mate preserves its own compression behavior during decompression; SPRING2 no longer collapses both output streams to one archive-wide gzip level.
- Fixed sorted-output quality and ID reordering after the in-memory reorder-to-encoder refactor by making downstream order-map generation stop once `read_order.bin` already covers all output positions, preventing stale singleton or N-order side files from corrupting reordered streams.
- Fixed archive extraction in `src/fs_utils.cpp` to reject absolute or escaping tar entry paths, preventing crafted archives from writing outside the requested extraction directory during preview, audit, decompression, or `SpringReader` setup.
- Fixed decompression output validation so SPRING2 now rejects colliding output paths and refuses to overwrite the input archive when reconstructing reads.
- Fixed grouped bundle decompression defaults so duplicate original lane basenames are deterministically disambiguated with role-based suffixes (`.R1`, `.R2`, `.R3`, `.I1`, `.I2`) instead of still colliding after a single `.index` suffix pass.
- Fixed grouped bundle manifest parsing to reject invalid `read3_alias_source` values and conflicting read-3 archive/alias declarations, preventing silent reconstruction of the wrong `R3` lane from malformed metadata.
- Fixed grouped bundle metadata validation drift by centralizing `bundle.meta` parsing and invariants in a shared helper used by compression, decompression, preview, and `SpringReader`; malformed grouped manifests and truncated nested member metadata now fail consistently.
- Fixed preview read-count reporting so archive metadata now labels the aggregate paired-end count as `Total Read Records` and reports per-input clean reads with explicit non-clean read counts, instead of presenting incompatible totals under `Reads Processed`.
- Fixed compression output validation so archive creation now refuses output paths that would overwrite any input FASTQ/FASTA file, preventing destructive in-place compression mistakes.
- Fixed `SpringReader` lifecycle handling by allowing the background producer to shut down cleanly when the reader is destroyed before the archive is fully consumed, avoiding a queue-backpressure deadlock on early exit.
- Fixed `SpringReader::get_digests()` so library callers can retrieve the actual computed sequence, quality, and ID CRCs after fully consuming an archive, instead of always receiving zeroed outputs.
- Fixed paired-end preprocess cleanup so merged mate-side `input_N.dna.2`, `read_order_N.bin.2`, and redundant mate-ID intermediates are closed before deletion and removed with the safe filesystem helper; this prevents stale raw temporary files from being packaged into `.sp` archives on Windows and reduces final archive size.
- Fixed release/install packaging drift so fresh self-contained builds no longer install vendored dependency artifacts or a standalone `rapidgzip` tool; Windows now defaults to static runtime linking, and a clean install produces a single `spring2.exe` instead of extra dependency binaries and headers.

## V1.0.0-beta

### Added

- Implemented **Archive Integrity Auditing**: Added record-lditing).
- Added `tests/integrity_test.cpp` to validate end-to-end data fidelity and corruption detection.
- Expanded the `SpringReader` API with `get_digests()` to enable programmatic integrity verification for library consumers.
- Implemented a public library-style **Streaming Decompression API** (`SpringReader`) for SPRING2 archives, enabling external tools to consume genomic records programmatically without intermediate file I/O.
- Added the `DecompressionSink` abstract interface, allowing the decompression engine to push reconstructed records to arbitrary consumers (files, memory buffers, or network streams).
- Integrated a high-performance **Asynchronous Producer-Consumer model** within `SpringReader` that leverages background pre-fetching to maintain maximum throughout during streaming.
- Added robust stream error checking in `src/decompress.cpp` to handle corrupt or truncated archives during position and orientation decoding.
- Replaced system `tar` subprocess calls with the native `libarchive` library, removing a major runtime dependency and improving cross-platform compatibility.
- Added `src/scoped_temp_file.h` providing `ScopedTempFile` RAII helper to safely remove temporary files in a noexcept destructor; replaces ad-hoc temp-file handling to reduce races and ensure deterministic cleanup.
- Added the `-V, --version` flag to both `spring2` and `spring2-preview` tools for reporting the application version.
- Integrated the **doctest** unit testing framework for granular logic verification.
- Added a suite of unit tests in `tests/unit_tests.cpp` covering core utility functions (sequence manipulation, parsing, string helpers).
- Integrated both unit and smoke tests into the unified `ctest` workflow.
- Implemented a native Windows backend for `MmapView` in `src/raii.h`, enabling high-performance memory-mapped I/O on Windows.
- Added automatic compiler-cache launcher support in CMake (`SPRING_ENABLE_COMPILER_CACHE`, default ON).
- Added automatic fast-linker selection in CMake (`SPRING_ENABLE_FAST_LINKER`, default ON), preferring `mold` on Linux and falling back to `lld` where supported.
- Added precompiled header support in CMake (`SPRING_ENABLE_PRECOMPILED_HEADERS`, default ON) to accelerate incremental rebuilds by caching stable headers (`src/pch.h`).
- Added upfront I/O parameter validation in `src/main.cpp` via `validate_io_parameters()` to verify input files exist, output directories are accessible, and paired-end compression requirements are met before entering the main compression/decompression pipeline; this ensures any runtime error is genuinely a compression/decompression issue, not a parameter error.

### Changed

- Optimized `reference_sequence_store::find_chunk_index` in `src/decompress.cpp` by replacing the linear scan with a binary search on chunk start offsets.
- **Improved compression parallelism** by restoring parallel sequence chunk packing in `src/encoder.cpp`, removing OpenMP critical sections from error logging in `src/encoder_impl.h` and `src/reorder_impl.h` (replaced with thread-local error aggregation), removing redundant startup barrier in `src/reorder_impl.h`, and unlocking MPHF dictionary thread count in `src/bitset_dictionary.h` (was hardcoded to 1).
- Increased and batched buffered I/O in the compression tail by expanding archive creation and per-thread encoder merge copy buffers to reduce serialized filesystem overhead.
- **Parallelized preprocessing N-read classification** in `src/preprocess.cpp` by adding `#pragma omp parallel for` loop for N-read detection to reduce preprocessing bottleneck on large inputs.
- Reduced preprocessing pass overhead by extending FASTQ/FASTA block parsing in `src/io_utils.cpp` to emit per-read metadata (length and N-presence), perform optional quality-length validation, and accumulate record CRCs while parsing; `src/preprocess.cpp` now consumes these parser outputs directly instead of re-scanning full record arrays.
- Reduced short-read preprocessing write overhead in `src/preprocess.cpp` by batching per-thread encoded clean/N-read payloads and flushing them once per block in deterministic order, and by batching quality/ID text stream writes into larger contiguous chunks.
- Reduced preprocessing allocation churn by conditionally allocating stream-specific string working sets and reusing per-step staging buffers in `src/preprocess.cpp`.
- Reduced sequential bottlenecks in `src/reordered_streams.cpp` by introducing safe bulk metadata loading (orientation/position/noise/read-length/order streams) with deterministic in-memory parsing, and by replacing per-read unaligned bit unpacking with boundary-validated parallel decode into disjoint output ranges.
- Reduced reorder lock contention in `src/reorder_impl.h` by shortening dictionary lock hold windows in `search_match`: candidate read IDs are snapshotted under lock and validated after unlock, reducing shard lock occupancy during Hamming/read-lock checks.
- Reduced shared-state contention in `src/reorder_impl.h` by splitting the unmatched-read fallback scan into per-thread striped seed buckets, so threads probe disjoint read windows with fewer lock collisions.
- Reduced synchronization and sorting overhead in the dictionary and reorder paths: `src/bitset_dictionary.h` now uses parallel chunk sorts with deterministic merges before the existing dedup pass, and `src/reorder_impl.h` assigns seed reads with OpenMP atomic capture instead of the initial cross-thread critical section.
- Optimized singleton DNA+N decoding in `src/encoder_impl.h` by replacing per-read `read_dnaN_from_bits()` calls with an inlined decoder loop and larger buffered stream I/O in the hot path.
- Optimized parallel gzip FASTQ writing in `src/util.cpp` by implementing thread-local reusable buffers and a persistent `libdeflate_compressor` cache to reduce heap allocator pressure.
- Optimized `merge_paired_n_reads` in `src/preprocess.cpp` to use buffered I/O, reducing the number of disk operations when merging N-read positions.
- Modernized the monolithic `compression_params` struct by decomposing it into nested, cohesive component structures (`EncodingConfig`, `QualityConfig`, `GzipMetadata`, `ReadMetadata`). This improves type safety and clarifies field ownership across the compression and decompression pipelines.
- Refactored `src/encoder.h` into a thin interface header, moving template implementations to `src/encoder_impl.h` to improve compilation performance and modularity.
- Added `src/raii.h` with RAII helpers (`OmpLock`, `OmpLockGuard`, `MmapView`); migrated `src/decompress.cpp`, `src/reorder_impl.h`, and `src/encoder_impl.h` to use RAII for `mmap` and OpenMP locks, and adopted RAII containers (`std::vector`, `std::unique_ptr`, `std::array`) across preprocessing and hot-path modules (e.g., `src/preprocess.cpp`, buffer and stream ownership in `src/decompress.cpp`) to replace manual `new[]/delete[]`, manage gzip streams, and improve resource safety.
- Hardened filesystem cleanup and temporary-file handling: added non-throwing `safe_remove_file()` and `safe_rename_file()` in `src/util.h`/`src/util.cpp` and replaced unchecked `remove()`/`rename()` call sites across `src/` with these helpers; added `src/scoped_temp_file.h` for noexcept temporary-file cleanup and made mapped-file RAII (`MmapView`) destructor `noexcept` to ensure safe unmapping during stack unwinding.
- Replaced hot-path raw arrays with `std::vector` or `std::unique_ptr<T[]>` starting in `src/reorder_impl.h` and `src/encoder_impl.h` to improve memory safety and eliminate manual `new[]/delete[]` usage.
- Replaced manual `char**` decoded noise table in `src/decompress.cpp` with `std::array<std::array<char,128>,128>`, removing manual `new[]/delete[]` and ensuring RAII-managed lifetime.
- Refactored `src/reorder.h` and the corresponding reordering implementation in `src/reorder_impl.h` to reduce template bloat and improve compilation speed.
- Modernized memory management in `bbhashdict` (the core read dictionary) by replacing manual `new[]/delete[]` with `std::unique_ptr` and `std::make_unique`.
- Refactored the `main.cpp` entry point to use a centralized RAII `SpringContext` class for managing temporary directories and resource cleanup, significantly reducing global state and improving signal-safety.
- Improved development documentation in `DEVELOPMENT.md` with a new section on unit testing.
- Refactored the monolithic `util.h` and `util.cpp` into a modular, domain-specific architecture:
  - `src/io_utils.h/cpp`: Specialized for C-SiT ID compression, QVZ quantization logic, and robust binary I/O.
  - `src/dna_utils.h/cpp`: Centralized DNA sequence manipulation (reverse complement, bit-packing, N-read encoding).
  - `src/parse_utils.h/cpp`: Handle ID pattern matching, FASTQ block parsing, and numeric string conversions.
  - `src/fs_utils.h/cpp`: Granular filesystem helpers and RAII file management.
  - `src/params.h/cpp`: Serialization of compression parameters and metadata structures to resolve circular dependencies.
- Refactored the core decompression engine in `src/decompress.cpp` and `src/decompress.h` into a stateful, sink-based architecture to support the new streaming reader while maintaining bit-perfect CLI backward compatibility.
- Upgraded the CLI verbosity system from a binary toggle to explicit log levels: default quiet mode keeps the progress bar, `--verbose`/`--verbose info` enables informational logs, and `--verbose debug` enables both informational and detailed debug diagnostics.
- Pruned indexed_bzip2 even more.
- Made windows build process guide easier and more straightforward.
- Expanded CMake runtime portability packaging: improved Windows GCC/OpenMP runtime DLL resolution and bundling for `spring2`, `spring2-preview`, and `rapidgzip`; added install-time RPATH configuration and OpenMP runtime library bundling support for macOS (`@loader_path/../lib`) and Linux (`$ORIGIN/../lib`).
- Consolidated all third-party dependency licenses into the central root `LICENSE` file for improved legal compliance and audit-readiness.

### Fixed

- Removed debug `[GZIP-DIAG]` logs from the compression pipeline that were firing even in non-verbose mode.
- Consolidated `parse_int_or_throw`, `parse_double_or_throw`, and `parse_uint64_or_throw` into `src/util.h` and `src/util.cpp` to remove duplication and potential ODR hazards.
- Consolidated `has_suffix` into `src/util.h` and `src/util.cpp`, removing duplicate definitions in `src/spring.cpp` and `src/decompress.cpp`.
- Removed a redundant duplicate call to `generatemasks` in the encoder initialization.
- Fixed an issue in `src/spring.cpp` where missing archive metadata could lead to silent decompression failures by adding validation for inferred output paths.
- Removed redundant dead code in decompression IO resolution logic.
- Fixed confusing error output in `src/main.cpp` by separating error handling for parameter validation errors (show help) from true runtime errors (show error message only); this ensures runtime failures are not obscured by help text.
- Hardened temporary directory cleanup in `SpringContext::cleanup()` by adding path canonicalization and boundary validation to prevent accidental recursive deletion if internal state is corrupted, and improved diagnostic logging for success/failure conditions.
- Eliminated dictionary lock contention during reorder search by implementing lock-free read-only access in `src/bitset_dictionary.h` and `src/reorder_impl.h`: after dictionary construction completes, `freeze()` marks the dictionary immutable, allowing all search operations to proceed without acquiring dictionary locks; this reduces lock wait time from ~168s to ~0, expected to improve reorder stage wall-clock by 18-25%.
- Fixed benchmark scripts (`tests/small_bench.ps1` and `tests/small_bench.sh`) to properly handle FASTQ files with IDs on the "+" quality separator line: FASTQ spec allows both "+ID" and "+" formats; SPRING2 normalizes to "+" (the canonical form) during compression. Benchmark scripts now normalize both original and restored files before comparison to ensure bit-perfect verification succeeds regardless of input "+" line format.

## V1.0.0-alpha

### Added

- Formally rebranded the software to **SPRING2**.
- Renamed the project and executable binary to `spring2`.
- Added a robust CMake `install` target to create a clean, portable distribution footprint (e.g., in a `dist/` or `spring2/` directory) containing only final binaries and required runtime libraries.
- Added benchmark scripts under `benchmark/` for lossless round-trip runs, comparison runs, and larger manual benchmarking workflows.
- Added round-trip integrity verification to the lossless benchmark flow, including checksum reporting when hashing tools are available.
- Added automatic support for gzipped compression inputs by staging `.gz` inputs into the temporary working directory before normal compression.
- Added automatic short-read versus long-read mode detection by pre-scanning input sequence lengths before compression.
- Added the `--memory` (`-m`) CLI option to conservatively reduce effective worker count on memory-constrained systems.
- Added a unified `-s, --strip [ioq]` CLI flag to discard identifiers (`i`), order (`o`), and quality scores (`q`), replacing independent flags.
- Added vendored `libdeflate` for fast whole-buffer DEFLATE, zlib, and gzip workloads used by the current build.
- Added vendored `rapidgzip` support for gzipped compression inputs through the pruned `indexed_bzip2` payload.
- Added a dedicated `dev/` tooling directory for repository maintenance, including linting, cppcheck, cbindgen validation, Valgrind smoke checks, shared helpers, and suppressions.
- Added configure-time `.clangd` generation so editor diagnostics inherit the active compiler's include paths and OpenMP configuration.
- Added extensive documentations.
- Added storage of the original input filenames and detailed gzip metadata (BGZF block size, header flags, MTIME, OS, and internal filename) within the compressed archive metadata.
- Added the `spring2-preview` utility for inspecting archive metadata, read counts, settings, and detailed compression ratios without full decompression.
- Added the `-n, --note` flag to store custom text notes within the archive.
- Added the `-u, --unzip` flag to force uncompressed output during decompression, even if the original input was gzipped.
- Added a specialized `-v, --verbose` flag to toggle between a real-time progress bar (default) and detailed informational logging.

### Changed

- Replaced the unmaintained legacy `id_comp` module with a natively integrated **Columnar Specialized Identifier Coder (C-SiT)** backed by Zstd (Level 22 max-compression). C-SiT dynamically parses FASTQ machine headers into dedicated columnar streams. For tile coordinates, it leverages an advanced auto-detecting Byte-Shuffled Delta Encoder that shrinks numeric identifiers into overlapping low-entropy sequences.
- Improved benchmark reporting so compression and decompression runs report elapsed time, CPU time, average core usage, and peak RSS when supported by the host environment.
- Changed the default thread selection logic to `min(max(1, hw_threads - 1), 16)`.
- Changed decompression output handling so output paths ending in `.gz` automatically produce gzipped FASTQ output.
- Replaced the `--gzip-level` option with a unified `-l, --level` flag (range 1–9, default 6). Values are passed to gzip for compressed output and scaled to Zstd (1–22) for internal streams.
- Renamed several core CLI flags for clarity and standard usage: `--num-threads` to `--threads` (`-t`), `--input-file` to `--input` (`-i`), `--output-file` to `--output` (`-o`), `--working-dir` to `--tmp-dir` (`-w`), and `--quality-opts` to `--qmod` (`-q`).
- Transitioned the recommended archive file extension from `.spring` to `.sp`.
- Removed the obsolete `--gzipped-fastq`, `--fasta-input`, and manual `-l` (long-read) flags.
- Repackaged `indexed_bzip2` into a smaller SPRING-specific archive payload that retains only the pieces needed for the current gzip workflow.
- Removed the final Boost dependency from the build and runtime path by replacing the remaining Boost-based gzip and mapped-file usage with local implementations.
- Upgraded the project toolchain baseline to C++20 and CMake 4.2.
- Refreshed the vendored dependency set used by the current tree, including `libbsc`, Cloudflare zlib, `libdeflate`, `qvz`, and the pruned `indexed_bzip2` payload.
- Replaced the unmaintained BBHash with a patched version of PTHash (making it more compatible with windows) for the hash table implementation.
- Made vendor extraction idempotent so repeated configure runs only re-extract archives when their content hash changes.
- Standardized formatting with the repository `.clang-format` configuration.
- Renamed several core source files to clearer role-based names, including `bitset_dictionary`, `template_dispatch`, `paired_end_order`, `reordered_quality_id`, and `reordered_streams`.
- Reorganized the internal implementation to reduce duplicated scaffolding across compression, decompression, preprocessing, template dispatch, and other core subsystems while preserving behavior.
- Accelerated the **QVZ lossy compression mode** by up to 1000x through a series of algorithmic optimizations:
  - Refactored the distortion matrix calculation from $O(N^2)$ to $O(N)$ using prefix sums.
  - Integrated a binary search for optimal quantization states and added a short-circuit path for ratio 1.0 targets.
  - Optimized the core probability propagation loops from $O(N^4)$ to $O(N^3)$ via invariant loop hoisting.

### Fixed

- Removed the previous Unix-only build assumption and enabled the modernized build and CI flow on Windows through a native MinGW-w64 toolchain.
- Fixed empty-reference-chunk decompression failures by switching the decoded chunk path to the local mapping wrapper.
- Reduced decompression peak memory usage by avoiding reconstruction of the full decoded reference in one large in-memory string.
- Reduced decompression write overhead by switching packed-sequence decode output to buffered block writes.
- Reduced compression-side sequence-packing overhead by eliminating the extra prepass and moving to buffered reads and writes.
- Reduced quality and identifier staging overhead by partitioning in a single pass and reloading batches more efficiently.
- Improved compiler portability by adding missing standard-library includes, replacing non-standard integer aliases with standard types, and fixing newer GCC compatibility issues in both first-party and vendored code.
- Fixed thread-count validation and related allocation hazards in CLI and internal helper paths.
- Fixed binary quality range validation for `-q binary` to avoid out-of-range table access and associated compiler warnings.
- Cleaned up lint and cppcheck findings across the current codebase, including missing initialization, binary I/O casts, signed-shift portability, and exception-safety issues.
- Tightened targeted lint-path handling and Python-file validation in the developer tooling.
- Stabilized the Valgrind smoke workflow by focusing failures on actionable leak classes and suppressing known OpenMP and libc startup noise.
- Fixed a memory corruption hazard in the `qvz` module by standardizing `ALPHABET_SIZE` at 128 to accommodate contemporary high-range Phred scores.
- Fixed a scope error in the `qvz` custom distortion matrix generator.
