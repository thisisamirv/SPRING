# dev/cppcheck.ps1 - C++ static analysis for SPRING2 development
$ErrorActionPreference = 'Stop'

# Import common utilities
. (Join-Path $PSScriptRoot "common.ps1")

# Locate cppcheck
Assert-Command "cppcheck"

# Configure include directories
$ID_COMPRESSION_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/id_compression/include")
$QVZ_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/qvz/include")
$PTHASH_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/pthash/include")
$ZSTD_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/zstd/lib")
$LIBBSC_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/libbsc")
$LIBDEFLATE_INCLUDE_DIR = (Join-Path $BUILD_DIR "vendor/libdeflate")

$INCLUDE_ARGS = @(
    "-I", (Join-Path $ROOT_DIR "src"),
    "-I", $VENDOR_ROOT,
    "-I", $ID_COMPRESSION_INCLUDE_DIR,
    "-I", $QVZ_INCLUDE_DIR,
    "-I", $PTHASH_INCLUDE_DIR,
    "-I", $ZSTD_INCLUDE_DIR,
    "-I", $LIBBSC_INCLUDE_DIR,
    "-I", $LIBDEFLATE_INCLUDE_DIR
)

# Configure targets
$targets = if ($args) { Get-FirstPartyPaths $args } else { $DEFAULT_CPP_ROOTS }

if ($null -eq $targets -or $targets.Count -eq 0) {
    Write-Error "No first-party targets selected for cppcheck."
    exit 1
}

# Run cppcheck
Write-Host "Running cppcheck on $($targets.Count) targets..." -ForegroundColor Cyan

# Prepare suppressions (using forward slashes for the BooPHF path to ensure cross-tool compatibility)
$BOOPHF_PATTERN = (Join-Path $ROOT_DIR "src/BooPHF.h").Replace('\', '/')

$cppcheckArgs = @(
    "--error-exitcode=1",
    "--enable=warning,performance,portability",
    "--suppress=missingInclude",
    "--suppress=missingIncludeSystem",
    "--suppress=*:$BOOPHF_PATTERN"
)

$cppcheckArgs += $INCLUDE_ARGS
$cppcheckArgs += $targets

# Use & (call operator) to invoke the native command
& cppcheck @cppcheckArgs
