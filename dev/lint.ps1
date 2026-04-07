# dev/lint.ps1 - Static analysis for SPRING2 development
$ErrorActionPreference = 'Stop'

# Import common utilities
. (Join-Path $PSScriptRoot "common.ps1")

# Locate clang-tidy
$clangTidyBin = ""
foreach ($candidate in @("clang-tidy", "clang-tidy-18", "clang-tidy-17", "clang-tidy-16")) {
    if (Test-Command $candidate) {
        $clangTidyBin = $candidate
        break
    }
}

if (-not $clangTidyBin) {
    Write-Error "Missing required command: clang-tidy"
    exit 1
}

$LINT_INCLUDE_DIR = Join-Path $ROOT_DIR "dev/include"
$TIDY_CHECKS = "*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr"

$ID_COMPRESSION_INCLUDE_DIR = Join-Path $BUILD_DIR "vendor/id_compression/include"
$QVZ_INCLUDE_DIR = Join-Path $BUILD_DIR "vendor/qvz/include"
$LIBDEFLATE_INCLUDE_DIR = Join-Path $BUILD_DIR "vendor/libdeflate"

$EXTRA_INCLUDES = @(
    (Join-Path $ROOT_DIR "src"),
    $VENDOR_ROOT,
    (Join-Path $BUILD_DIR "vendor"),
    $ID_COMPRESSION_INCLUDE_DIR,
    $LIBDEFLATE_INCLUDE_DIR,
    $QVZ_INCLUDE_DIR
)

$commonTidyArgs = @(
    "-quiet",
    "-checks=$TIDY_CHECKS",
    "-header-filter=^$",
    "--system-headers=false",
    "--extra-arg-before=--target=x86_64-w64-windows-gnu",
    "--extra-arg-before=--driver-mode=g++",
    "--extra-arg=-I$LINT_INCLUDE_DIR",
    "--extra-arg=-w",
    "--extra-arg=-Wno-unknown-argument",
    "--extra-arg=-Wno-unknown-warning-option"
)

# Collect files from args or default locations
$cppFiles = Get-CppSources $args
$pythonFiles = Get-PythonSources $args

if ($null -eq $cppFiles -and $null -eq $pythonFiles) {
    Write-Host "No source files found to lint." -ForegroundColor Yellow
    exit 0
}

if ($cppFiles) {
    # On Windows, we'll try to find compile_commands.json
    $hasCompileCommands = $false
    $tidyDbDir = $BUILD_DIR

    if (Test-Path $COMPILE_COMMANDS) {
        Write-Host "Sanitizing compilation database for clang-tidy..." -ForegroundColor Gray
        $sanitizedContent = Get-Content $COMPILE_COMMANDS -Raw
        
        # Strip GCC module flags that Clang driver errors on
        $sanitizedContent = $sanitizedContent -replace '-fmodules-ts', ''
        $sanitizedContent = $sanitizedContent -replace '-fmodule-mapper=[^ "]+', ''
        $sanitizedContent = $sanitizedContent -replace '-fdeps-format=[^ "]+', ''
        
        $tidyDbDir = Join-Path $BUILD_DIR "tidy_db"
        if (-not (Test-Path $tidyDbDir)) { New-Item -ItemType Directory -Path $tidyDbDir -Force | Out-Null }
        
        $sanitizedContent | Set-Content (Join-Path $tidyDbDir "compile_commands.json") -Encoding UTF8
        $hasCompileCommands = $true
        Write-Host "Using sanitized compilation database at: $tidyDbDir" -ForegroundColor Gray
    }

    $compileDbFiles = @()
    $standaloneFiles = @()

    foreach ($file in $cppFiles) {
        if ($hasCompileCommands -and (Test-CompileCommandsContains $file)) {
            $compileDbFiles += $file
        } else {
            $standaloneFiles += $file
        }
    }

    if ($compileDbFiles) {
        Write-Host "Linting $($compileDbFiles.Count) files via compilation database..." -ForegroundColor Cyan
        $tidyArgs = $commonTidyArgs + @("-p", "$tidyDbDir") + $compileDbFiles
        
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'SilentlyContinue'
        & $clangTidyBin @tidyArgs 2>&1 | ForEach-Object {
            $line = $_.ToString()
            if ($line -notmatch '^\d+ warnings? generated\.$|^\d+ warnings? and \d+ errors? generated\.$|^\[\d+/\d+\] Processing file') {
                Write-Host $line
            }
        }
        $ErrorActionPreference = $oldPreference
    }

    if ($standaloneFiles) {
        Write-Host "Linting $($standaloneFiles.Count) standalone files..." -ForegroundColor Cyan
        foreach ($file in $standaloneFiles) {
            Write-Host "Linting $file..." -ForegroundColor Gray
            $includeArgs = @()
            $includeArgs += "-I$LINT_INCLUDE_DIR"
            foreach ($inc in $EXTRA_INCLUDES) {
                $includeArgs += "-I$inc"
            }
            $includeArgs += "-I$((Get-Item $file).DirectoryName)"

            $tidyArgs = $commonTidyArgs + @($file, "--", "-std=c++20", "-x", "c++") + $includeArgs
            
            $oldPreference = $ErrorActionPreference
            $ErrorActionPreference = 'SilentlyContinue'
            & $clangTidyBin @tidyArgs 2>&1 | ForEach-Object {
                $line = $_.ToString()
                if ($line -notmatch '^\d+ warnings? generated\.$|^\d+ warnings? and \d+ errors? generated\.$|^\[\d+/\d+\] Processing file') {
                    Write-Host $line
                }
            }
            $ErrorActionPreference = $oldPreference
        }
    }
}

if ($pythonFiles) {
    Assert-Command "python"
    Write-Host "Linting $($pythonFiles.Count) Python files..." -ForegroundColor Cyan
    foreach ($file in $pythonFiles) {
        Write-Host "Linting $file..." -ForegroundColor Gray
        python -m py_compile $file
    }
}
