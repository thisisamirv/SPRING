# Changelog

## [2026-04-02]

### Performance & Benchmarking

* **Added a Lossless Round-Trip Benchmark Harness**: Introduced `benchmark/run_lossless_benchmark.sh` to run Spring lossless compression against a FASTQ input, auto-detect long-read mode, store outputs under `benchmark/output/`, use per-input scratch space under `benchmark/scratch/`, and report compressed size reduction and compression ratio.

* **Expanded Benchmark Resource Reporting**: Extended the benchmark harness to build Spring quietly when needed, capture separate compression and decompression resource usage, report elapsed time, CPU time, average core usage, and peak RSS when GNU `time` is available, and degrade cleanly when only shell timing is present.

* **Added Round-Trip Integrity Verification**: Updated the benchmark harness to decompress the generated `.spring` payload, compare the output byte-for-byte against the original FASTQ, and report SHA-256 checksums when checksum tools are available.

* **Reduced Compression-Side Sequence Packing Overhead**: Reworked `src/encoder.cpp` so packed-sequence chunks are generated in a single pass instead of performing a separate base-count prepass before packing, reducing redundant sequence traversal during lossless compression.

* **Buffered Sequence Packing I/O**: Further updated `src/encoder.cpp` so packed-sequence generation uses buffered binary reads and buffered packed-byte writes instead of per-character formatted extraction and one-byte output writes, reducing stream overhead in the sequence-packing stage while preserving the encoded output.

* **Removed Repeated Quality/ID Batch Rescans**: Reworked `src/reordered_quality_id.cpp` so quality and identifier streams are partitioned into temporary batch files in a single sequential pass and then compressed batch-by-batch, eliminating the previous full-file rescan for every reorder batch during compression.

* **Reduced Quality/ID Batch Staging I/O Overhead**: Updated `src/reordered_quality_id.cpp` so temporary batch files use buffered writes during partitioning and whole-file batch reads during reload, reducing syscall churn in the reorder-and-compress staging path while preserving lossless output.

* **Auto-Detected Gzipped Compression Inputs**: Updated `src/spring.cpp` so `.fastq.gz` compression inputs are detected by filename, decompressed into the per-run temporary directory, and then fed through the normal FASTQ compression path without requiring any extra CLI flag.

* **Switched Gzipped Decompression Output to Filename-Based Behavior**: Updated `src/decompress.cpp` so decompression automatically emits gzipped FASTQ when the requested output path ends in `.gz`, while preserving the existing `--gzip-level` control for compression level selection.

* **Removed the Obsolete `-g` CLI Flag**: Simplified `src/main.cpp` and the Spring CLI surface by removing the old `--gzipped-fastq/-g` option now that gzipped compression inputs are auto-detected and gzipped decompression output is inferred from the output filename.

* **Added libdeflate for Whole-Buffer Compression Workloads**: Integrated vendored `libdeflate` so Spring can use its fast in-memory DEFLATE, zlib, and gzip compression/decompression routines for the non-streaming compression paths, reducing reliance on heavier external compression components while keeping zlib-compatible streaming support where it is still required.

* **Integrated rapidgzip for Gzipped Compression Inputs**: Wired a pruned vendored `rapidgzip` build from `indexed_bzip2` into Spring's `.fastq.gz` compression-input staging path, using Spring's existing thread count for decoder parallelism and retaining the zlib-based fallback if the external decoder fails.

* **Pruned and Repackaged indexed_bzip2 as an Archive Payload**: Reduced the vendored `indexed_bzip2` tree to the Spring-specific gzip-only, non-Python `rapidgzip` subset, removed unused tests/benchmarks/examples/zlib-ng/rpmalloc/bzip2/Python tooling and extra CLI features, then repackaged the retained sources as `vendor/indexed_bzip2.tar.xz` so CMake now extracts the dependency into the build tree instead of depending on an unpacked source directory in the repository.

* **Removed the Final Boost Dependency**: Replaced the remaining Boost.Iostreams gzip and mapped-file usage with local zlib-backed gzip wrappers and a POSIX `mmap`-based reference-chunk reader, then removed Boost from the top-level `CMakeLists.txt` so Spring now builds without linking any Boost components.

* **Removed the Unused Vendored Boost Archive**: Deleted the stale `vendor/boost.tar.xz` payload and removed its license-manifest entry now that the Spring build no longer references any Boost components.

* **Fixed Decompression Failures on Empty Reference Chunks**: Updated `src/decompress.cpp` so decoded packed-sequence chunks are opened through the local mapping wrapper, which safely handles zero-length chunk files produced by all-singleton blocks instead of aborting on Boost's zero-length mapped-file error path.

* **Reduced Decompression Peak Memory Usage**: Reworked `src/decompress.cpp` to avoid rebuilding the full decoded reference into one large in-memory string, using chunk-backed reference access instead so large reference data can be read on demand during decompression.

* **Reduced Decompression Write Overhead**: Updated `src/decompress.cpp` to replace char-by-char packed-sequence decode output with buffered block writes, reducing per-character stream overhead while preserving lossless FASTQ reconstruction.

## [2026-04-01]

### Build System & Tooling

* **Updated libbsc to Latest Version**: Replaced the `src/libbsc` internal library with its latest upstream version. Ensured preservation of project-specific API wrappers (`bsc.h`, `bsc.cpp`, `bsc_str_array.cpp`) to maintain compatibility with the Spring codebase.

* **Made Configure-Time Vendor Extraction Idempotent**: Updated the top-level `CMakeLists.txt` so vendored archives under `vendor/` are only re-extracted when their archive hash changes, and the Cloudflare zlib configure/build step is only rerun when its extracted tree or `libz.a` is missing. Repeated `cmake -S . -B build` runs in an existing build directory now complete cleanly instead of requiring a fresh build tree after dependency changes.

* **Pruned and Compressed Vendored Dependencies**: Reduced repository size by removing non-essential vendored materials such as unused documentation, examples, tests, CI assets, and other unneeded packaging content where appropriate, and by repackaging retained third-party dependencies as local archive payloads under `vendor/`. These reductions were made to support distribution and build efficiency while preserving required license notices and the project-specific compatibility adjustments needed by the Spring codebase.

* **Updated cloudflare-zlib**: Synchronized the bundled zlib dependency with its latest configuration.

* **Updated BBHash**: Updated BooPHF.h script to the latest version of BBHash.

* **Upgraded Python 2 to Python 3**: Migrated all Python utilities to **Python 3**. Standardized indentation and updated `print` statements to resolve `SyntaxError` and `TabError` issues.

* **Upgraded project from C++11 to C++20**: Uphraded the C++ version to support more modern features.

* **Upgraded CMake Requirements**: Bumped the main project to **CMake 4.2** and aligned the temporary Boost integration path with modern CMake behavior while completing the dependency refresh.

* **Optimized Main Configuration**: Reordered the top-level `CMakeLists.txt` to ensure `cmake_minimum_required` precedes the `project()` initialization, resolving implicit developer warnings.

* **Dynamic IDE Support**: Implemented automated `.clangd` generation within the CMake configuration. This dynamically queries the active compiler for its internal include paths to resolve OpenMP headers (`omp.h`) generically across different Linux environments and GCC versions.

* **Standardized Code Style**: Integrated a project-wide `.clang-format` configuration based on the **LLVM** standard. Systematically applied this style across the entire codebase to ensure consistent indentation, brace placement, and overall readability.

* **Restored CMake 4.3 Compatibility for Vendored Boost**: Updated `vendor/boost-cmake/CMakeLists.txt` to set policy `CMP0169` to `OLD`, allowing the bundled Boost build logic to continue using `FetchContent_Populate` under newer CMake releases.

* **Migrated to Native Boost 1.90 CMake**: Replaced the old `boost-cmake` wrapper and bundled `boost_1_67_0.tar.xz` payload with the local `vendor/boost/boost-1.90.0-cmake.tar.xz` source archive, using Boost's own CMake build and removing the obsolete wrapper directory from the repository.

* **Reorganized Repository Utilities**: Split the old mixed-purpose `util/` directory into `tests` for bundled regression assets and smoke checks, `scripts/analysis` and `scripts/preprocessing` for standalone helpers, and removed the obsolete archival benchmark and dependency-probe leftovers.

* **Removed Broken Legacy Benchmark Scripts**: Deleted archived benchmark shell scripts that no longer work with the current repository because they hardcoded external `/raid/...` datasets, depended on missing third-party comparator installs, or referenced obsolete custom `spring_*` binaries.

* **Removed Broken Legacy Dependency Probes**: Deleted the old `tests/legacy` Boost/OpenMP probe sources after confirming they no longer compile cleanly against the current project include layout, while retaining the remaining self-contained legacy analysis helpers that still build independently.

* **Flattened Tests and Analysis Helpers**: Moved the smoke assets and runner from `tests/smoke` into `tests/`, folded `scripts/analysis/legacy` into `scripts/analysis`, and removed the now-unused `benchmarks/` directory entirely.

* **Added Developer Tooling Directory**: Introduced a dedicated `dev/` directory for repository maintenance scripts. It now contains shared path/build helpers in `common.sh`, static analysis entrypoints in `lint.sh` and `cppcheck.sh`, the smoke-test Valgrind wrapper in `valgrind-smoke.sh`, and the corresponding Valgrind suppressions in `valgrind.supp`.

* **Cleaned Up Static Analysis Tooling**: Tuned `dev/lint.sh` and `dev/cppcheck.sh` for actionable first-party checks, broadened them to cover `src/`, `scripts/`, and `tests/`, suppressed bundled `src/BooPHF.h` noise in `cppcheck`, and fixed the reported Spring-owned issues including binary I/O cast cleanup, missing member initialization in owning/template-heavy headers, an uninitialized flag in stream reordering, signed-shift portability in zigzag encoding, and exception-safe `compression_params` lifetime handling.
* **Extended Lint Coverage to Python Scripts**: Updated `dev/common.sh` and `dev/lint.sh` so the lint wrapper now discovers Python files under `scripts/`, `tests/`, and `dev/`, validates them with `python3 -m py_compile`, and only requires the CMake compilation database when C/C++ targets are actually being linted.

* **Fixed Targeted Lint Path Handling**: Normalized repo-relative paths in `dev/common.sh` so direct invocations such as `./dev/lint.sh src/bitset_dictionary.cpp` resolve against `compile_commands.json` correctly instead of falling back to standalone-file handling.

* **Stabilized Valgrind Smoke Checks**: Updated `dev/valgrind-smoke.sh` to treat only definite and indirect leaks as failures, added targeted suppressions for known OpenMP/glibc TLS startup noise in `dev/valgrind.supp`, and restored a clean `Tests successful!` smoke run without masking actionable memory leaks.

### Compiler Compatibility

* **GCC 13+ Compliance**: Resolved "undeclared identifier" errors for standard integer types (such as `uint32_t` and `int64_t`) and standard templates (like `std::vector`) by adding explicit `#include <cstdint>` and `#include <vector>` across all core headers and source files:
  * `src/util.h`
  * `src/bitset_dictionary.h`
  * `src/libbsc/bsc.h`
  * `src/params.h`
  * `src/BooPHF.h`
  * `src/encoder.h`
  * `src/decompress.h`
  * `src/reorder.h`
  * `src/spring.h`
  * `src/id_compression/include/id_compression.h` (added `"id_compression/include/stream_model.h"`)
  * `src/id_compression/src/io_functions.cpp` (added `<cstring>`)

* **Standard Type Migration**: Systematically replaced non-standard BSD/Linux type aliases (specifically `u_int64_t`) with the standard C++11 `uint64_t` across the entire codebase to ensure cross-compiler compatibility.

* **BSC Library Fixes**: Added missing `<string>` and `<cstdint>` inclusions to `src/libbsc/bsc.h` to ensure standalone header validity in modern C++ environments.

* **GCC 15 Compatibility for id_compression**: Added missing `<cstring>` includes to `vendor/id_compression/src/id_compression.cpp` and `vendor/id_compression/src/sam_file_allocation.cpp` so `strcpy`, `memcpy`, and `memset` resolve correctly with newer GCC toolchains.

* **GCC 15 Allocation Warning Cleanup**: Eliminated `-Walloc-size-larger-than` warnings by validating positive thread counts in `src/main.cpp`, replacing thread-sized scratch buffers in `src/util.cpp`, `src/preprocess.cpp`, `src/encoder.h`, and `src/reorder.h` with standard containers, and tightening thread-count handling in `src/BooPHF.h`.
* **Preprocess and BooPHF Hardening**: Completed the follow-up sweep in `src/preprocess.cpp` and `src/BooPHF.h` by converting remaining thread-indexed scratch storage to standard containers and enforcing positive thread counts in internal helper paths.

* **Header Cleanup**: Removed redundant `#includes` to satisfy modern linters and improve compilation efficiency:
  * `"params.h"` from `src/bitset_dictionary.h`.
  * `"algorithm"` from `src/encoder.cpp`.
  * `"cstdio"` and `"fstream"` from `src/main.cpp`.
  * `"string"` from `src/params.h`.
  * `"cmath"` from `src/preprocess.cpp`.
  * `"algorithm"` and `"id_compression/include/sam_block.h"` from `src/reordered_quality_id.cpp`.
  * `"cstdint"`, `"stdexcept"`, and `"vector"` from `src/reordered_streams.cpp`.
  * `"encoder.h"` and `"reorder.h"` from `src/spring.cpp`.
  * `"iostream"` from `scripts/analysis/illumina_quality_binning.cpp`.
  * `"string.h"` from `src/qvz/include/codebook.h`.
  * `"stdint.h"`, `"stdio.h"`, `"string.h"`, `"time.h"`, and `"qvz/include/codebook.h"` from `src/qvz/include/qv_compressor.h`.
  * `"float.h"` and `"time.h"` from `src/qvz/include/util.h`.
  * `"stdio.h"`, `"qvz/include/util.h"`, and `"string.h"` from `src/qvz/src/cluster.cpp`.
  * `"fstream"` from `src/qvz/src/codebook.cpp`.
  * `"qvz/include/util.h"` from `src/qvz/src/distortion.cpp`.
  * `"stdio.h"`, `"string.h"`, and `"qvz/include/util.h"` from `src/qvz/src/lines.cpp`.
  * `"fstream"` and `"string"` from `src/qvz/src/qv_compressor.cpp`.
  * `"fstream"`, `"iostream"`, `"stdio.h"`, `"string.h"`, and `"qvz/include/util.h"` from `src/qvz/src/qvz.cpp`.
  * `"ctype.h"` and `"string.h"` from `src/id_compression/include/Arithmetic_stream.h`.
  * `"id_compression/include/sam_block.h"` from `src/id_compression/include/id_compression.h`.
  * `"string.h"` from `src/id_compression/include/sam_block.h`.

### Bug Fixes

* Resolved "undefined behavior" warning in `src/qvz/src/pmf.cpp` by replacing the `NAN` macro with `std::numeric_limits<double>::quiet_NaN()`. Updated `src/qvz/include/util.h` to use standard C++ definitions for `INFINITY` and `NAN` and addressed a "header not used directly" lint warning for `<limits>`.
* Fixed "undeclared identifier" and "incomplete type" errors in `src/qvz/src/qv_compressor.cpp` by adding missing `#include <cstdint>`, `"qvz/include/codebook.h"`, and `"qvz/include/lines.h"`.
* Fixed "undeclared identifier" error for `DBL_MAX` in `src/qvz/src/quantizer.cpp` by replacing it with `std::numeric_limits<double>::max()`.
* Updated legacy test utility `util/boost_openmp_bcm_test.cpp` to reference the correct `libbsc/bsc.h` header and `spring::bsc` namespace, resolving a "file not found" error for the outdated `bcm/bcm.h`.
* Fixed "Non-void function does not return a value" errors in `util/io_experiments.cpp` by converting experimental functions to `void` and ensuring `main` returns a valid integer.
* Resolved "undeclared identifier" errors for standard integer types in `util/` by adding missing `#include <cstdint>` to several utility files including `quality_value_counts.cpp`, `pack_quality_values.cpp`, `unpack_quality_values.cpp`, `illumina_quality_binning.cpp`, and `quality_run_length_encode.cpp`.
* **id_compression Maintenance**: Commented out unimplemented `compress_rname` and `decompress_rname` declarations in `id_compression.h` that relied on the undefined `rname_models` type, resolving a blocking compilation error.
* **Boost Linker Fix**: Linked `Boost::system` explicitly in the top-level `CMakeLists.txt` so the `spring` target links successfully when using the bundled static Boost build with `Boost::filesystem`.
* **Thread Count Validation**: Added explicit checks that thread counts are positive in CLI and internal helper paths, preventing signed-to-unsigned size underflow in thread-indexed allocations.
* **Binary Quality Range Validation**: Bounded binary quality binning to the 128-entry ASCII table and rejected out-of-range `-q binary` parameters, eliminating GCC 15 `-Wstringop-overflow` warnings in preprocessing.
* **Refactored bbhashdict Bin-State Handling**: Reorganized `src/bitset_dictionary.cpp` to simplify sentinel-based bin range tracking in `findpos` and `remove`, reducing repeated index arithmetic and clarifying the deletion-state transitions without changing behavior.
* **Organized Bitset Dictionary Construction**: Reworked `src/bitset_dictionary.h` to use clearer type aliases, shared temporary-file path helpers, and per-dictionary local references inside `constructdictionary`, reducing repeated bookkeeping and making the hash-dictionary build path easier to follow without changing behavior.
* **Organized BooPHF Level Processing**: Reworked the Spring-local `src/BooPHF.h` integration to use a shared temporary-level filename helper, clearer type aliases, and simplified file-lifecycle branching inside `mphf::processLevel`, making the on-disk level-processing path easier to follow without changing behavior.
* **Organized Template Dispatch Entry Points**: Reworked `src/template_dispatch.cpp` to replace the large reorder and encoder switch ladders with explicit dispatcher tables plus shared bitset-size validation, keeping the supported template instantiations unchanged while making the entry-point selection logic easier to follow.
* **Reduced Template Dispatch Header Coupling**: Simplified `src/template_dispatch.h` into a lighter declaration-only header by forward-declaring `compression_params` instead of pulling in `util.h`, reducing unnecessary header dependencies without changing the public interface.
* **Organized Decompression Scaffolding**: Reworked `src/decompress.cpp` to centralize repeated block-file path construction, `.bsc` decompression setup, output-file opening, and per-step read-count clamping into local helpers, reducing duplicated control-flow scaffolding while keeping the short- and long-read decompression logic unchanged.
* **Reduced Decompression Header Coupling**: Simplified `src/decompress.h` into a lighter declaration-only header by forward-declaring `compression_params` instead of including `util.h`, reducing unnecessary transitive dependencies without changing the public interface.
* **Organized Encoder File Handling**: Reworked `src/encoder.cpp` to centralize repeated per-thread file-path construction and local file lifecycle handling in the sequence-packing and order-correction paths, reducing duplicated string-building and cleanup logic without changing encoding behavior.
* **Organized Encoder Header Setup**: Reworked `src/encoder.h` to simplify `encoder_main` object lifetime management, centralize encoder dictionary range initialization in a shared helper, and reduce repeated singleton-pool bookkeeping, making the template entry path easier to follow without changing behavior.
* **Organized Parameter Definitions**: Reworked `src/params.h` into grouped `constexpr` configuration constants for read limits, reorder tuning, encoder tuning, and block sizing, preserving the existing names and values while making the shared parameter header easier to scan.
* **Organized Paired-End Order Remapping**: Reworked `src/paired_end_order.cpp` to replace manual heap arrays with standard containers and clarify the intermediate paired-end order-mapping variables, making the reorder-to-output remap logic easier to follow without changing behavior.
* **Reduced Paired-End Header Coupling**: Simplified `src/paired_end_order.h` into a lighter declaration-only header by forward-declaring `compression_params` instead of including `util.h`, reducing unnecessary transitive dependencies without changing the public interface.
* **Organized Preprocess I/O Setup**: Reworked `src/preprocess.cpp` to centralize gzip input stream setup/reset/teardown, block-file path construction, and repeated block-number bookkeeping, reducing duplicated preprocessing scaffolding while keeping the FASTQ/FASTA preprocessing behavior unchanged.
* **Reduced Preprocess Header Coupling**: Simplified `src/preprocess.h` into a lighter declaration-only header by forward-declaring `compression_params` instead of including `util.h`, reducing unnecessary transitive dependencies without changing the public interface.
* **Organized Reorder Compression Flow**: Reworked `src/reordered_quality_id.cpp` to replace raw temporary arrays with standard containers, centralize block-file path handling, and move private order-generation and compression helpers into internal scope with clearer mode handling, making the reorder-and-compress flow easier to follow without changing behavior.
* **Reduced Reorder Compression Header Coupling**: Simplified `src/reordered_quality_id.h` into a lighter public header by forward-declaring `compression_params` and removing declarations for file-local helper functions that are only used inside the implementation.
* **Organized Reorder Stream Compression**: Reworked `src/reordered_streams.cpp` to centralize block-file path construction and repeated compress-and-remove steps, while replacing manual temporary arrays with standard containers for stream metadata and payload buffers, making the reorder stream emission path easier to follow without changing behavior.
* **Reduced Reorder Stream Header Coupling**: Simplified `src/reordered_streams.h` into a lighter declaration-only header by forward-declaring `compression_params` instead of including `util.h`, reducing unnecessary transitive dependencies without changing the public interface.
* **Organized Reorder Header Setup**: Reworked `src/reorder.h` to simplify `reorder_main` object lifetime management, centralize reorder dictionary range initialization in a shared helper, and tighten `reorder_global` initialization with `nullptr`, making the template entry path easier to follow without changing behavior.
* **Organized Top-Level Spring Orchestration**: Reworked `src/spring.cpp` to centralize repeated step timing/reporting and shell-command execution in local helpers, reducing duplicated orchestration scaffolding across compression, archiving, untarring, and decompression while keeping the overall workflow unchanged.
* **Organized Utility Serialization Helpers**: Reworked `src/util.cpp` to centralize repeated FASTQ record emission, paired-ID pattern checks, DNA bit-packing/unpacking, and reverse-complement logic in shared local helpers, making the core utility layer easier to follow without changing behavior; also tightened `src/util.h` with an explicit `cstddef` include for its `size_t` declarations.
* **Organized Spring Public Header**: Reworked `src/spring.h` into a cleaner declaration-only header by introducing small aliases for repeated string-list and decompression-range types and adding an explicit `cstddef` include for `size_t`, reducing signature noise without changing the public API behavior.
* **Organized Main Entrypoint Flow**: Reworked `src/main.cpp` to centralize temporary-directory creation and cleanup, isolate compress-vs-decompress dispatch in a helper, and reduce repeated error-handling scaffolding around the command-line entry path, making the top-level control flow easier to follow without changing behavior.
* **Organized Quality Count Analysis Script**: Reworked `scripts/analysis/quality_value_counts.cpp` to remove global input/output state, replace raw multidimensional heap allocations with flat vectors plus index helpers, and centralize binary output emission in a helper, making the standalone quality-counting script easier to follow without changing its output format.
* **Organized Illumina Quality Binning Script**: Reworked `scripts/analysis/illumina_quality_binning.cpp` to replace the mutable global lookup table with a local `std::array` helper result and pass the table explicitly into the binning routine, reducing global state while keeping the standalone binning behavior unchanged.
* **Organized Quality Packing Script**: Reworked `scripts/analysis/pack_quality_values.cpp` to replace the ad hoc in-function lookup table with a local `std::array` helper, isolate file-length, chunk-packing, and tail-writing steps into small helpers, and keep the packed-byte plus tail-file output format unchanged.
* **Organized Quality Transpose Script**: Reworked `scripts/analysis/transpose_quality_matrix.cpp` to isolate argument parsing, input loading, and transposed output writing into small helpers, making the standalone quality-matrix transpose flow easier to follow without changing the output layout.
* **Organized Quality Unpack Script**: Reworked `scripts/analysis/unpack_quality_values.cpp` to move the decode table and packed-byte expansion logic into small local helpers and switch the packed-input loop to a read-driven form, keeping the `.packed` plus `.packed.tail` reconstruction behavior unchanged.
* **Organized Read-Length Distribution Script**: Reworked `scripts/analysis/read_length_distribution.cpp` to replace the manual fixed-size C array with a local `std::array` counter, isolate FASTQ sequence-line selection in a helper, and separate counting from printing, making the standalone read-length distribution script easier to follow without changing its tabular output.
* **Organized Quality RLE Script**: Reworked `scripts/analysis/quality_run_length_encode.cpp` to centralize run-length emission in small helpers and simplify the main encoding loop with an early-continue structure, keeping the `.rl.len` and `.rl.char` outputs unchanged.
* **Organized Long-Read Split Script**: Reworked `scripts/preprocessing/split_long_reads.cpp` to isolate FASTQ-record reading, read/quality length validation, and chunk emission into small helpers, making the long-read splitting flow easier to follow without changing the generated `.split` output.
* **Organized Entropy Analysis Script**: Reworked `scripts/analysis/per_position_noise_entropy.py` to separate read counting, probability accumulation, entropy computation, and final reporting into clearer helpers, reducing inline bookkeeping while keeping the entropy and size calculations unchanged.
* **Organized Noise Entropy Analysis Script**: Reworked `scripts/analysis/noise_entropy.py` to move the top-level execution flow into a `main()` entry point, centralize repeated cluster-weighted reporting logic in helpers, and factor the 0th/1st/2nd order summaries into clearer phases while keeping the numerical analysis unchanged.
* **Organized Quality Entropy Analysis Script**: Reworked `scripts/analysis/quality_entropy.py` to move the active execution path into a `main()` entry point and isolate the 0th/1st/2nd order summary calculations and printing into small helpers, keeping the reported entropy values unchanged while reducing top-level inline math.
* **Organized Top-Level CMake Build Script**: Reworked `CMakeLists.txt` to gather source files with grouped `list(APPEND ...)` sections, centralize repeated developer-target creation in a small helper function, and consolidate Spring's Boost link declarations, making the build script easier to scan without changing build behavior.
* **Renamed Ambiguous Core Source Files**: Renamed several `src/` files to clearer role-based names, including `bitset_util` to `bitset_dictionary`, `call_template_functions` to `template_dispatch`, `pe_encode` to `paired_end_order`, `reorder_compress_quality_id` to `reordered_quality_id`, and `reorder_compress_streams` to `reordered_streams`, while updating includes, include guards, and CMake source registration to match.
* **Renamed Ambiguous Helper Scripts**: Renamed several `scripts/` helpers to clearer role-based names, including `compute_entropy` to `per_position_noise_entropy`, `quality_counts` to `quality_value_counts`, `quality_illumina_bin` to `illumina_quality_binning`, `quality_pack` to `pack_quality_values`, `quality_transpose` to `transpose_quality_matrix`, `quality_unpack` to `unpack_quality_values`, `readlendistr` to `read_length_distribution`, `RLE` to `quality_run_length_encode`, and `splitlongreads` to `split_long_reads`.

* **Renamed Ambiguous Variable Names**: Renamed several ambigous variable names to more clear descriptive names.

* **Added Documentation**: Added a brief documentation to each script.