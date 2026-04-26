# dev/cppcheck.ps1 - C++ static analysis for SPRING2 development
$ErrorActionPreference = 'Stop'

# Import common utilities
. (Join-Path $PSScriptRoot "common.ps1")

# Locate cppcheck
Assert-Command "cppcheck"

# Configure include directories
$ZSTD_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/zstd/lib")
$LIBBSC_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/libbsc")
$LIBDEFLATE_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/libdeflate")
$LIBARCHIVE_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/libarchive/lib")
$ZLIB_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/cloudflare_zlib")
$BZIP2_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/indexed_bzip2/src")
$PTHASH_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/pthash/include")
$PTHASH_EXTERNAL_DIR = (Join-Path $ROOT_DIR "vendor/pthash/external")
$QVZ_INCLUDE_DIR = (Join-Path $ROOT_DIR "vendor/qvz/include")

$INCLUDE_ARGS = @(
    "-I", (Join-Path $ROOT_DIR "src"),
    "-I", (Join-Path $ROOT_DIR "vendor"),
    "-I", $ZSTD_INCLUDE_DIR,
    "-I", $LIBBSC_INCLUDE_DIR,
    "-I", $LIBDEFLATE_INCLUDE_DIR,
    "-I", $LIBARCHIVE_INCLUDE_DIR,
    "-I", $ZLIB_INCLUDE_DIR,
    "-I", $BZIP2_INCLUDE_DIR,
    "-I", $PTHASH_INCLUDE_DIR,
    "-I", (Join-Path $PTHASH_EXTERNAL_DIR "xxHash"),
    "-I", (Join-Path $PTHASH_EXTERNAL_DIR "bits/include"),
    "-I", (Join-Path $PTHASH_EXTERNAL_DIR "bits/external/essentials/include"),
    "-I", (Join-Path $PTHASH_EXTERNAL_DIR "mm_file/include"),
    "-I", $QVZ_INCLUDE_DIR
)

# Configure targets
$targets = if ($args) { $args | ForEach-Object { Resolve-RepoPath $_ } } else {
    @(
        (Join-Path $ROOT_DIR "src"),
        (Join-Path $ROOT_DIR "vendor"),
        (Join-Path $ROOT_DIR "tests")
    )
}

if ($null -eq $targets -or $targets.Count -eq 0) {
    Write-Error "No targets selected for cppcheck."
    exit 1
}

# Run cppcheck
Write-Host "Running cppcheck on $($targets.Count) targets..." -ForegroundColor Cyan

$cppcheckArgs = @(
    "--error-exitcode=1",
    "--enable=warning,performance,portability",
    "--suppress=missingInclude",
    "--suppress=missingIncludeSystem",
    "--suppress=normalCheckLevelMaxBranches",
    "--suppress=toomanyconfigs",
    "--suppress=*:$($ROOT_DIR.Replace('\', '/') + '/tests/doctest.h')",
    "--suppress=preprocessorErrorDirective:*vendor/libarchive/*",
    "--suppress=syntaxError:*vendor/libarchive/*",
    "--suppress=sizeofwithnumericparameter:*vendor/libarchive/*",
    "--suppress=nullPointerRedundantCheck:*vendor/libarchive/*",
    "--suppress=memleak:*vendor/libarchive/*",
    "--suppress=uninitvar:*vendor/libarchive/*",
    "--suppress=pointerSize:*vendor/libarchive/*",
    "--suppress=literalWithCharPtrCompare:*vendor/libarchive/*",
    "--suppress=internalAstError:*vendor/libarchive/*",
    "--suppress=unknownMacro:*vendor/libarchive/*",
    "--suppress=resourceLeak:*vendor/libbsc/*",
    "--suppress=preprocessorErrorDirective:*vendor/libbsc/*",
    "--suppress=nullPointerOutOfMemory:*vendor/qvz/*",
    "--suppress=duplInheritedMember:*vendor/indexed_bzip2/*",
    "--suppress=identicalConditionAfterEarlyExit:*vendor/indexed_bzip2/src/rapidgzip/chunkdecoding/GzipChunk.hpp",
    "--suppress=sameIteratorExpression:*vendor/indexed_bzip2/src/core/FasterVector.hpp",
    "--suppress=identicalInnerCondition:*vendor/libbsc/filters/detectors.cpp",
    "--suppress=legacyUninitvar:*vendor/libbsc/st/st.cpp",
    "--suppress=arrayIndexOutOfBoundsCond:*vendor/libdeflate/*",
    "--suppress=unknownMacro:*vendor/pthash/*",
    "--suppress=ctunullpointerOutOfMemory:*vendor/qvz/*",
    "--suppress=ctuuninitvar:*vendor/libarchive/*"
)

$cppcheckArgs += $INCLUDE_ARGS
$cppcheckArgs += $targets

# Run cppcheck and hide per-file "Checking ..." chatter while keeping
# progress lines and diagnostics.
& cppcheck @cppcheckArgs 2>&1 | ForEach-Object {
    $line = $_.ToString()
    if ($line -notmatch '^\s*Checking\s+') {
        Write-Output $line
    }
}

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
