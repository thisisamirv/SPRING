# Changelog

## [2026-04-01]

### Build System & Tooling

* **Updated libbsc to Latest Version**: Replaced the `src/libbsc` internal library with its latest upstream version. Ensured preservation of project-specific API wrappers (`bsc.h`, `bsc.cpp`, `bsc_str_array.cpp`) to maintain compatibility with the Spring codebase.
* **Updated cloudflare-zlib**: Synchronized the bundled zlib dependency with its latest configuration.

* **Upgraded Python 2 to Python 3**: Migrated all Python utilities to **Python 3**. Standardized indentation and updated `print` statements to resolve `SyntaxError` and `TabError` issues.

* **Upgraded CMake Requirements**: Bumped the main project to **CMake 4.2** and aligned the temporary Boost integration path with modern CMake behavior while completing the dependency refresh.

* **Optimized Main Configuration**: Reordered the top-level `CMakeLists.txt` to ensure `cmake_minimum_required` precedes the `project()` initialization, resolving implicit developer warnings.

* **Dynamic IDE Support**: Implemented automated `.clangd` generation within the CMake configuration. This dynamically queries the active compiler for its internal include paths to resolve OpenMP headers (`omp.h`) generically across different Linux environments and GCC versions.

* **Standardized Code Style**: Integrated a project-wide `.clang-format` configuration based on the **LLVM** standard. Systematically applied this style across the entire codebase to ensure consistent indentation, brace placement, and overall readability.

* **Restored CMake 4.3 Compatibility for Vendored Boost**: Updated `third_party/boost-cmake/CMakeLists.txt` to set policy `CMP0169` to `OLD`, allowing the bundled Boost build logic to continue using `FetchContent_Populate` under newer CMake releases.

* **Migrated to Native Boost 1.90 CMake**: Replaced the old `boost-cmake` wrapper and bundled `boost_1_67_0.tar.xz` payload with the local `third_party/boost/boost-1.90.0-cmake.tar.xz` source archive, using Boost's own CMake build and removing the obsolete wrapper directory from the repository.

* **Reorganized Repository Utilities**: Split the old mixed-purpose `util/` directory into `tests` for bundled regression assets and smoke checks, `scripts/analysis` and `scripts/preprocessing` for standalone helpers, and removed the obsolete archival benchmark and dependency-probe leftovers.

* **Removed Broken Legacy Benchmark Scripts**: Deleted archived benchmark shell scripts that no longer work with the current repository because they hardcoded external `/raid/...` datasets, depended on missing third-party comparator installs, or referenced obsolete custom `spring_*` binaries.

* **Removed Broken Legacy Dependency Probes**: Deleted the old `tests/legacy` Boost/OpenMP probe sources after confirming they no longer compile cleanly against the current project include layout, while retaining the remaining self-contained legacy analysis helpers that still build independently.

* **Flattened Tests and Analysis Helpers**: Moved the smoke assets and runner from `tests/smoke` into `tests/`, folded `scripts/analysis/legacy` into `scripts/analysis`, and removed the now-unused `benchmarks/` directory entirely.

* **Added Developer Tooling Directory**: Introduced a dedicated `dev/` directory for repository maintenance scripts. It now contains shared path/build helpers in `common.sh`, static analysis entrypoints in `lint.sh` and `cppcheck.sh`, the smoke-test Valgrind wrapper in `valgrind-smoke.sh`, and the corresponding Valgrind suppressions in `valgrind.supp`.

* **Cleaned Up Static Analysis Tooling**: Tuned `dev/lint.sh` and `dev/cppcheck.sh` for actionable first-party checks, broadened them to cover `src/`, `scripts/`, and `tests/`, suppressed bundled `src/BooPHF.h` noise in `cppcheck`, and fixed the reported Spring-owned issues including binary I/O cast cleanup, missing member initialization in owning/template-heavy headers, an uninitialized flag in stream reordering, signed-shift portability in zigzag encoding, and exception-safe `compression_params` lifetime handling.

* **Stabilized Valgrind Smoke Checks**: Updated `dev/valgrind-smoke.sh` to treat only definite and indirect leaks as failures, added targeted suppressions for known OpenMP/glibc TLS startup noise in `dev/valgrind.supp`, and restored a clean `Tests successful!` smoke run without masking actionable memory leaks.

### Compiler Compatibility

* **GCC 13+ Compliance**: Resolved "undeclared identifier" errors for standard integer types (such as `uint32_t` and `int64_t`) and standard templates (like `std::vector`) by adding explicit `#include <cstdint>` and `#include <vector>` across all core headers and source files:
  * `src/util.h`
  * `src/bitset_util.h`
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

* **GCC 15 Compatibility for id_compression**: Added missing `<cstring>` includes to `third_party/id_compression/src/id_compression.cpp` and `third_party/id_compression/src/sam_file_allocation.cpp` so `strcpy`, `memcpy`, and `memset` resolve correctly with newer GCC toolchains.

* **GCC 15 Allocation Warning Cleanup**: Eliminated `-Walloc-size-larger-than` warnings by validating positive thread counts in `src/main.cpp`, replacing thread-sized scratch buffers in `src/util.cpp`, `src/preprocess.cpp`, `src/encoder.h`, and `src/reorder.h` with standard containers, and tightening thread-count handling in `src/BooPHF.h`.
* **Preprocess and BooPHF Hardening**: Completed the follow-up sweep in `src/preprocess.cpp` and `src/BooPHF.h` by converting remaining thread-indexed scratch storage to standard containers and enforcing positive thread counts in internal helper paths.

* **Header Cleanup**: Removed redundant `#includes` to satisfy modern linters and improve compilation efficiency:
  * `"params.h"` from `src/bitset_util.h`.
  * `"algorithm"` from `src/encoder.cpp`.
  * `"cstdio"` and `"fstream"` from `src/main.cpp`.
  * `"string"` from `src/params.h`.
  * `"cmath"` from `src/preprocess.cpp`.
  * `"algorithm"` and `"id_compression/include/sam_block.h"` from `src/reorder_compress_quality_id.cpp`.
  * `"cstdint"`, `"stdexcept"`, and `"vector"` from `src/reorder_compress_streams.cpp`.
  * `"encoder.h"` and `"reorder.h"` from `src/spring.cpp`.
  * `"iostream"` from `util/quality_illumina_bin.cpp`.
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
* Resolved "undeclared identifier" errors for standard integer types in `util/` by adding missing `#include <cstdint>` to several utility files including `quality_counts.cpp`, `quality_pack.cpp`, `quality_unpack.cpp`, `quality_illumina_bin.cpp`, and `RLE.cpp`.
* **id_compression Maintenance**: Commented out unimplemented `compress_rname` and `decompress_rname` declarations in `id_compression.h` that relied on the undefined `rname_models` type, resolving a blocking compilation error.
* **Boost Linker Fix**: Linked `Boost::system` explicitly in the top-level `CMakeLists.txt` so the `spring` target links successfully when using the bundled static Boost build with `Boost::filesystem`.
* **Thread Count Validation**: Added explicit checks that thread counts are positive in CLI and internal helper paths, preventing signed-to-unsigned size underflow in thread-indexed allocations.
* **Binary Quality Range Validation**: Bounded binary quality binning to the 128-entry ASCII table and rejected out-of-range `-q binary` parameters, eliminating GCC 15 `-Wstringop-overflow` warnings in preprocessing.
