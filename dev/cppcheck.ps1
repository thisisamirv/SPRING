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

# Skip tests from cppcheck to avoid third-party doctest noise.
$testsDirPath = [System.IO.Path]::GetFullPath((Join-Path $ROOT_DIR "tests"))
$filteredTargets = @()
foreach ($target in $targets) {
    $fullTarget = [System.IO.Path]::GetFullPath($target)
    if ($fullTarget -ieq $testsDirPath -or
        $fullTarget.StartsWith($testsDirPath + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $filteredTargets += $target
}
$targets = $filteredTargets

if ($null -eq $targets -or $targets.Count -eq 0) {
    Write-Error "No first-party targets selected for cppcheck."
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
    "--suppress=toomanyconfigs"
)

$cppcheckArgs += $INCLUDE_ARGS
$cppcheckArgs += $targets

# Use & (call operator) to invoke the native command
& cppcheck @cppcheckArgs
