# Changelog

## [2026-04-01]

### Build System & Tooling

* **Updated libbsc to Latest Version**: Replaced the `src/libbsc` internal library with its latest upstream version. Ensured preservation of project-specific API wrappers (`bsc.h`, `bsc.cpp`, `bsc_str_array.cpp`) to maintain compatibility with the Spring codebase.
* **Updated cloudflare-zlib**: Synchronized the bundled zlib dependency with its latest configuration.

* **Upgraded Python 2 to Python 3**: Migrated all Python utilities to **Python 3**. Standardized indentation and updated `print` statements to resolve `SyntaxError` and `TabError` issues.

* **Upgraded CMake Requirements**: Bumped minimum CMake version to **4.2** across the main project and `boost-cmake` dependency to align with modern standards and silence deprecation warnings.

* **Optimized Main Configuration**: Reordered the top-level `CMakeLists.txt` to ensure `cmake_minimum_required` precedes the `project()` initialization, resolving implicit developer warnings.

* **Dynamic IDE Support**: Implemented automated `.clangd` generation within the CMake configuration. This dynamically queries the active compiler for its internal include paths to resolve OpenMP headers (`omp.h`) generically across different Linux environments and GCC versions.

* **Standardized Code Style**: Integrated a project-wide `.clang-format` configuration based on the **LLVM** standard. Systematically applied this style across the entire codebase to ensure consistent indentation, brace placement, and overall readability.

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
