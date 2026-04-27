# dev/lint.ps1 - Static analysis for SPRING2 development
$ErrorActionPreference = 'Stop'

# Import common utilities
. (Join-Path $PSScriptRoot "common.ps1")

$VENDOR_ROOT = Join-Path $ROOT_DIR "vendor"

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
$TIDY_CHECKS = "*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr,-misc-use-anonymous-namespace,-cppcoreguidelines-pro-type-union-access,-bugprone-macro-parentheses,-bugprone-implicit-widening-of-multiplication-result,-readability-avoid-nested-conditional-operator,-hicpp-deprecated-headers,-modernize-deprecated-headers,-misc-redundant-expression,-cppcoreguidelines-avoid-goto,-hicpp-avoid-goto,-modernize-redundant-void-arg,-readability-redundant-casting,-readability-inconsistent-declaration-parameter-name,-clang-analyzer-core.BitwiseShift,-bugprone-casting-through-void,-cppcoreguidelines-use-enum-class,-performance-enum-size,-readability-uppercase-literal-suffix,-readability-redundant-parentheses,-bugprone-assignment-in-if-condition,-modernize-use-bool-literals,-bugprone-inc-dec-in-conditions,-clang-analyzer-core.NullPointerArithm,-hicpp-function-size,-readability-function-size,-clang-analyzer-deadcode.DeadStores,-clang-analyzer-core.uninitialized.Assign,-bugprone-reserved-identifier,-performance-no-int-to-ptr,-bugprone-suspicious-string-compare,-hicpp-multiway-paths-covered,-readability-redundant-member-init,-readability-container-contains,-misc-no-recursion,-readability-duplicate-include,-bugprone-signed-char-misuse,-modernize-avoid-variadic-functions,-bugprone-multi-level-implicit-pointer-conversion,-readability-avoid-unconditional-preprocessor-if,-clang-analyzer-core.UndefinedBinaryOperatorResult,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.strcpy,-readability-redundant-preprocessor,-misc-confusable-identifiers,-modernize-use-designated-initializers,-portability-simd-intrinsics,-modernize-use-integer-sign-comparison,-misc-unused-parameters,-readability-suspicious-call-argument,-hicpp-no-assembler,-readability-avoid-return-with-void-value,-clang-analyzer-unix.Stream,-modernize-concat-nested-namespaces,-readability-convert-member-functions-to-static,-bugprone-unchecked-string-to-number-conversion,-performance-inefficient-vector-operation,-clang-analyzer-core.NullDereference,-portability-avoid-pragma-once,-cppcoreguidelines-non-private-member-variables-in-classes,-cppcoreguidelines-special-member-functions,-hicpp-special-member-functions,-readability-redundant-string-init,-modernize-use-nodiscard,-cppcoreguidelines-use-default-member-init,-modernize-use-default-member-init,-readability-const-return-type,-cppcoreguidelines-avoid-const-or-ref-data-members,-performance-unnecessary-value-param,-misc-anonymous-namespace-in-header,-readability-qualified-auto,-hicpp-use-emplace,-modernize-use-emplace,-boost-use-ranges,-modernize-use-ranges,-readability-avoid-const-params-in-decls,-readability-redundant-access-specifiers,-readability-redundant-typename,-bugprone-throwing-static-initialization,-cppcoreguidelines-pro-type-const-cast,-bugprone-unintended-char-ostream-output,-modernize-use-constraints,-misc-override-with-different-visibility,-bugprone-derived-method-shadowing-base-method,-bugprone-unchecked-optional-access,-cppcoreguidelines-rvalue-reference-param-not-moved,-clang-analyzer-optin.portability.UnixAPI,-bugprone-suspicious-stringview-data-usage,-clang-analyzer-unix.StdCLibraryFunctions,-hicpp-exception-baseclass,-misc-throw-by-value-catch-by-reference,-bugprone-string-literal-with-embedded-nul,-modernize-use-scoped-lock,-cppcoreguidelines-misleading-capture-default-by-value,-cppcoreguidelines-pro-type-static-cast-downcast,-android-cloexec-dup,-misc-multiple-inheritance,-readability-use-anyofallof,-modernize-type-traits,-cppcoreguidelines-missing-std-forward,-cppcoreguidelines-explicit-virtual-functions,-hicpp-use-override,-modernize-use-override"

$ZSTD_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/zstd"
$LIBBSC_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libbsc"
$LIBDEFLATE_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libdeflate"
$LIBARCHIVE_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libarchive/lib"
$ZLIB_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/cloudflare_zlib"
$BZIP2_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/indexed_bzip2"
$BZIP2_ISAL_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/indexed_bzip2/isa-l/include"
$QVZ_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/qvz"
$PTHASH_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/pthash/include"
$PTHASH_EXTERNAL_DIR = Join-Path $ROOT_DIR "vendor/pthash/external"

$EXTRA_INCLUDES = @(
    (Join-Path $ROOT_DIR "src"),
    $VENDOR_ROOT,
    $ZSTD_INCLUDE_DIR,
    $LIBBSC_INCLUDE_DIR,
    $LIBDEFLATE_INCLUDE_DIR,
    $LIBARCHIVE_INCLUDE_DIR,
    $ZLIB_INCLUDE_DIR,
    $BZIP2_INCLUDE_DIR,
    $BZIP2_ISAL_INCLUDE_DIR,
    $QVZ_INCLUDE_DIR,
    $PTHASH_INCLUDE_DIR,
    (Join-Path $PTHASH_EXTERNAL_DIR "xxHash"),
    (Join-Path $PTHASH_EXTERNAL_DIR "bits/include"),
    (Join-Path $PTHASH_EXTERNAL_DIR "bits/external/essentials/include"),
    (Join-Path $PTHASH_EXTERNAL_DIR "mm_file/include")
)

$commonTidyArgs = @(
    "-quiet",
    "-checks=$TIDY_CHECKS",
    "-header-filter=^$",
    "--system-headers=false",
    "--extra-arg-before=--target=x86_64-w64-windows-gnu",
    "--extra-arg=-I$LINT_INCLUDE_DIR",
    "--extra-arg=-isystem",
    "--extra-arg=$ROOT_DIR/tests",
    "--extra-arg=-w",
    "--extra-arg=-Wno-unknown-argument",
    "--extra-arg=-Wno-unknown-warning-option",
    "--extra-arg=-fconstexpr-steps=4194304",
    "--extra-arg=-fopenmp"
)

# Collect files from args or default locations
$lintTargets = if ($args) { $args } else {
    @(
        (Join-Path $ROOT_DIR "src"),
        (Join-Path $ROOT_DIR "vendor"),
        (Join-Path $ROOT_DIR "tests")
    )
}
$cppFiles = Get-CppSources $lintTargets
$pythonFiles = Get-PythonSources $lintTargets

if ($null -eq $cppFiles -and $null -eq $pythonFiles) {
    Write-Host "No source files found to lint." -ForegroundColor Yellow
    exit 0
}

if ($cppFiles) {
    Assert-BuildDir

    # Prefer compilation database when available; generate it if missing.
    $hasCompileCommands = $false
    $tidyDbDir = $BUILD_DIR

    if (-not (Test-Path $COMPILE_COMMANDS)) {
        Write-Host "compile_commands.json not found; re-running CMake configure with export enabled..." -ForegroundColor Yellow
        $cmakeResult = & cmake -S $ROOT_DIR -B $BUILD_DIR -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to generate compile_commands.json via CMake.`n$cmakeResult"
            exit 1
        }
    }

    if (Test-Path $COMPILE_COMMANDS) {
        Write-Host "Sanitizing compilation database for clang-tidy..." -ForegroundColor Gray
        $sanitizedContent = Get-Content $COMPILE_COMMANDS -Raw
        
        # Strip GCC module flags that Clang driver errors on
        $sanitizedContent = $sanitizedContent -replace '-fmodules-ts', ''
        $sanitizedContent = $sanitizedContent -replace '-fmodule-mapper=[^ "]+', ''
        $sanitizedContent = $sanitizedContent -replace '-fdeps-format=[^ "]+', ''
        # Strip CMake PCH include flags so clang-tidy does not consume stale
        # compiler-version-specific .pch artifacts.
        $sanitizedContent = $sanitizedContent -replace '-include-pch\s+"?[^"]*cmake_pch\.(hxx|h)\.pch"?', ''
        $sanitizedContent = $sanitizedContent -replace '-include\s+"?[^"]*cmake_pch\.(hxx|h)"?', ''
        $sanitizedContent = $sanitizedContent -replace '-include-pch"?[^"]*cmake_pch\.(hxx|h)\.pch"?', ''
        $sanitizedContent = $sanitizedContent -replace '-include"?[^"]*cmake_pch\.(hxx|h)"?', ''
        $sanitizedContent = $sanitizedContent -replace '\s+', ' '
        
        $tidyDbDir = Join-Path $BUILD_DIR "tidy_db"
        if (-not (Test-Path $tidyDbDir)) { New-Item -ItemType Directory -Path $tidyDbDir -Force | Out-Null }
        
        $sanitizedContent | Set-Content (Join-Path $tidyDbDir "compile_commands.json") -Encoding UTF8
        $hasCompileCommands = $true
        Write-Host "Using sanitized compilation database at: $tidyDbDir" -ForegroundColor Gray
    }
    else {
        Write-Error "Expected compilation database at $COMPILE_COMMANDS"
        exit 1
    }

    $compileDbFiles = @()
    $standaloneFiles = @()

    foreach ($file in $cppFiles) {
        if ($hasCompileCommands -and (Test-CompileCommandsContains $file)) {
            $compileDbFiles += $file
        }
        else {
            $standaloneFiles += $file
        }
    }

    if ($hasCompileCommands -and -not $compileDbFiles -and $standaloneFiles) {
        Write-Host "No files matched compile_commands.json directly; falling back to compilation-database mode for all files." -ForegroundColor Yellow
        $compileDbFiles = @($standaloneFiles)
        $standaloneFiles = @()
    }

    if ($compileDbFiles) {
        Write-Host "Linting $($compileDbFiles.Count) files via compilation database..." -ForegroundColor Cyan
        $compileDbIncludeArgs = @()
        foreach ($inc in $EXTRA_INCLUDES) {
            $compileDbIncludeArgs += "--extra-arg=-I$inc"
        }
        $tidyArgs = $commonTidyArgs + @("-p", "$tidyDbDir") + $compileDbIncludeArgs + $compileDbFiles
        
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

            $tidyArgs = $commonTidyArgs + @($file, "--", "--driver-mode=g++", "-std=c++20", "-x", "c++") + $includeArgs
            
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
