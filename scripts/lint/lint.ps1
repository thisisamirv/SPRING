# scripts/lint/lint.ps1 - Static analysis for SPRING2 development
$ErrorActionPreference = 'Stop'

# Import common utilities
. (Join-Path $PSScriptRoot "..\common\common.ps1")

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

$LINT_INCLUDE_DIR = Join-Path $ROOT_DIR "scripts/lint/include"
$TIDY_CHECKS = "*,-fuchsia-*,-llvmlibc-*,-altera-*,-google-*,-cert-*,-llvm-*,-cppcoreguidelines-avoid-magic-numbers,-readability-magic-numbers,-misc-const-correctness,-readability-identifier-length,-bugprone-empty-catch,-misc-include-cleaner,-modernize-use-trailing-return-type,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-misc-use-internal-linkage,-readability-isolate-declaration,-readability-math-missing-parentheses,-modernize-return-braced-init-list,-concurrency-mt-unsafe,-misc-non-private-member-variables-in-classes,-bugprone-random-generator-seed,-bugprone-narrowing-conversions,-cppcoreguidelines-narrowing-conversions,-hicpp-explicit-conversions,-hicpp-named-parameter,-readability-named-parameter,-performance-avoid-endl,-cppcoreguidelines-macro-usage,-cppcoreguidelines-macro-to-enum,-modernize-macro-to-enum,-readability-use-concise-preprocessor-directives,-modernize-use-using,-modernize-avoid-c-style-cast,-cppcoreguidelines-pro-type-cstyle-cast,-cppcoreguidelines-pro-type-reinterpret-cast,-cppcoreguidelines-owning-memory,-cppcoreguidelines-no-malloc,-hicpp-no-malloc,-cppcoreguidelines-avoid-c-arrays,-hicpp-avoid-c-arrays,-modernize-avoid-c-arrays,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-pro-type-vararg,-hicpp-vararg,-hicpp-signed-bitwise,-cppcoreguidelines-init-variables,-openmp-use-default-none,-readability-function-cognitive-complexity,-bugprone-easily-swappable-parameters,-modernize-loop-convert,-bugprone-too-small-loop-variable,-readability-static-accessed-through-instance,-readability-use-std-min-max,-readability-container-data-pointer,-readability-make-member-function-const,-hicpp-braces-around-statements,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-portability-template-virtual-member-function,-hicpp-use-auto,-modernize-use-auto,-readability-redundant-control-flow,-performance-unnecessary-copy-initialization,-hicpp-use-nullptr,-modernize-use-nullptr,-readability-implicit-bool-conversion,-readability-non-const-parameter,-readability-else-after-return,-cppcoreguidelines-pro-type-member-init,-hicpp-member-init,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-hicpp-no-array-decay,-android-cloexec-fopen,-openmp-exception-escape,-abseil-string-find-str-contains,-cppcoreguidelines-avoid-non-const-global-variables,-bugprone-exception-escape,-bugprone-signal-handler,-performance-inefficient-string-concatenation,-bugprone-branch-clone,-bugprone-switch-missing-default-case,-bugprone-command-processor,-misc-predictable-rand,-hicpp-uppercase-literal-suffix,-readability-container-size-empty,-cppcoreguidelines-avoid-do-while,-clang-analyzer-security.ArrayBound,-readability-simplify-boolean-expr,-misc-use-anonymous-namespace,-cppcoreguidelines-pro-type-union-access,-bugprone-macro-parentheses,-bugprone-implicit-widening-of-multiplication-result,-readability-avoid-nested-conditional-operator,-hicpp-deprecated-headers,-modernize-deprecated-headers,-misc-redundant-expression,-cppcoreguidelines-avoid-goto,-hicpp-avoid-goto,-modernize-redundant-void-arg,-readability-redundant-casting,-readability-inconsistent-declaration-parameter-name,-clang-analyzer-core.BitwiseShift,-bugprone-casting-through-void,-cppcoreguidelines-use-enum-class,-performance-enum-size,-readability-uppercase-literal-suffix,-readability-redundant-parentheses,-bugprone-assignment-in-if-condition,-modernize-use-bool-literals,-bugprone-inc-dec-in-conditions,-clang-analyzer-core.NullPointerArithm,-hicpp-function-size,-readability-function-size,-clang-analyzer-deadcode.DeadStores,-clang-analyzer-core.uninitialized.Assign,-bugprone-reserved-identifier,-performance-no-int-to-ptr,-bugprone-suspicious-string-compare,-hicpp-multiway-paths-covered,-readability-redundant-member-init,-readability-container-contains,-misc-no-recursion,-readability-duplicate-include,-bugprone-signed-char-misuse,-modernize-avoid-variadic-functions,-bugprone-multi-level-implicit-pointer-conversion,-readability-avoid-unconditional-preprocessor-if,-clang-analyzer-core.UndefinedBinaryOperatorResult,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.strcpy,-readability-redundant-preprocessor,-misc-confusable-identifiers,-modernize-use-designated-initializers,-portability-simd-intrinsics,-modernize-use-integer-sign-comparison,-misc-unused-parameters,-readability-suspicious-call-argument,-hicpp-no-assembler,-readability-avoid-return-with-void-value,-clang-analyzer-unix.Stream,-modernize-concat-nested-namespaces,-readability-convert-member-functions-to-static,-bugprone-unchecked-string-to-number-conversion,-performance-inefficient-vector-operation,-clang-analyzer-core.NullDereference,-portability-avoid-pragma-once,-cppcoreguidelines-non-private-member-variables-in-classes,-cppcoreguidelines-special-member-functions,-hicpp-special-member-functions,-readability-redundant-string-init,-modernize-use-nodiscard,-cppcoreguidelines-use-default-member-init,-modernize-use-default-member-init,-readability-const-return-type,-cppcoreguidelines-avoid-const-or-ref-data-members,-performance-unnecessary-value-param,-misc-anonymous-namespace-in-header,-readability-qualified-auto,-hicpp-use-emplace,-modernize-use-emplace,-boost-use-ranges,-modernize-use-ranges,-readability-avoid-const-params-in-decls,-readability-redundant-access-specifiers,-readability-redundant-typename,-bugprone-throwing-static-initialization,-cppcoreguidelines-pro-type-const-cast,-bugprone-unintended-char-ostream-output,-modernize-use-constraints,-misc-override-with-different-visibility,-bugprone-derived-method-shadowing-base-method,-bugprone-unchecked-optional-access,-cppcoreguidelines-rvalue-reference-param-not-moved,-clang-analyzer-optin.portability.UnixAPI,-bugprone-suspicious-stringview-data-usage,-clang-analyzer-unix.StdCLibraryFunctions,-hicpp-exception-baseclass,-misc-throw-by-value-catch-by-reference,-bugprone-string-literal-with-embedded-nul,-modernize-use-scoped-lock,-cppcoreguidelines-misleading-capture-default-by-value,-cppcoreguidelines-pro-type-static-cast-downcast,-android-cloexec-dup,-misc-multiple-inheritance,-readability-use-anyofallof,-modernize-type-traits,-cppcoreguidelines-missing-std-forward,-cppcoreguidelines-explicit-virtual-functions,-hicpp-use-override,-modernize-use-override,-modernize-use-std-numbers"

$ZSTD_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/zstd"
$LIBBSC_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libbsc"
$LIBDEFLATE_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libdeflate"
$LIBARCHIVE_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/libarchive/lib"
$ZLIB_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/cloudflare_zlib"
$BZIP2_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/indexed_bzip2"
$BZIP2_ISAL_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/indexed_bzip2/isa-l/include"
$QVZ_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/qvz"
$PTHASH_INCLUDE_DIR = Join-Path $ROOT_DIR "vendor/pthash"

$EXTRA_INCLUDES = @(
    (Join-Path $ROOT_DIR "src"),
    (Join-Path $ROOT_DIR "src/common"),
    (Join-Path $ROOT_DIR "src/assays"),
    (Join-Path $ROOT_DIR "src/decompress"),
    (Join-Path $ROOT_DIR "src/encode"),
    (Join-Path $ROOT_DIR "src/preprocess"),
    (Join-Path $ROOT_DIR "src/reorder"),
    (Join-Path $ROOT_DIR "src/workflow"),
    $VENDOR_ROOT,
    $ZSTD_INCLUDE_DIR,
    $LIBBSC_INCLUDE_DIR,
    $LIBDEFLATE_INCLUDE_DIR,
    $LIBARCHIVE_INCLUDE_DIR,
    $ZLIB_INCLUDE_DIR,
    $BZIP2_INCLUDE_DIR,
    $BZIP2_ISAL_INCLUDE_DIR,
    $QVZ_INCLUDE_DIR,
    $PTHASH_INCLUDE_DIR
)

$commonTidyArgs = @(
    "-quiet",
    "-checks=$TIDY_CHECKS",
    "-header-filter=^$",
    "--system-headers=false"
)

# Platform-specific clang-tidy arguments (align with scripts/lint/lint.sh)
$isMsysWindows = $false
if ($env:MSYSTEM) { $isMsysWindows = $true }
$hostIsMacOS = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)
$hostIsWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)

if ($isMsysWindows -or $hostIsWindows) {
    $commonTidyArgs += "--extra-arg-before=--target=x86_64-w64-windows-gnu"
    $commonTidyArgs += "--extra-arg=-I$LINT_INCLUDE_DIR"
    $commonTidyArgs += "--extra-arg=-fopenmp"
    $commonTidyArgs += "--extra-arg=-w"
    $commonTidyArgs += "--extra-arg=-fconstexpr-steps=4194304"
}
elseif ($hostIsMacOS) {
    if (Test-Command "xcrun") {
        try { $MACOS_SDK_PATH = & xcrun --sdk macosx --show-sdk-path 2>$null } catch { $MACOS_SDK_PATH = $null }
        try { $apple_clangxx_path = & xcrun --sdk macosx --find clang++ 2>$null } catch { $apple_clangxx_path = $null }
        if ($apple_clangxx_path) { $apple_toolchain_usr_dir = Split-Path -Parent (Split-Path -Parent $apple_clangxx_path); $MACOS_CXX_INCLUDE_DIR = Join-Path $apple_toolchain_usr_dir "include/c++/v1" }
    }
    $commonTidyArgs += "--extra-arg=-w"
    $commonTidyArgs += "--extra-arg=-fconstexpr-steps=4194304"
    if ($MACOS_SDK_PATH) { $commonTidyArgs += "--extra-arg-before=-isysroot"; $commonTidyArgs += "--extra-arg-before=$MACOS_SDK_PATH"; $commonTidyArgs += "--extra-arg-before=-stdlib=libc++" }
    if ($MACOS_CXX_INCLUDE_DIR -and (Test-Path $MACOS_CXX_INCLUDE_DIR)) { $commonTidyArgs += "--extra-arg-before=-isystem"; $commonTidyArgs += "--extra-arg-before=$MACOS_CXX_INCLUDE_DIR" }
}
else {
    # Linux-like host
    $LINUX_GCC_TOOLCHAIN_DIR = $null
    if (Test-Command "g++") {
        try { $libgcc = & g++ -print-libgcc-file-name 2>$null; if ($libgcc) { $LINUX_GCC_TOOLCHAIN_DIR = Split-Path -Parent (Split-Path -Parent $libgcc) } } catch {}
    }
    $LINUX_CLANG_RESOURCE_INCLUDE_DIR = $null
    foreach ($candidate in @("clang++", "clang++-18", "clang++-17", "clang++-16")) { if (Test-Command $candidate) { $clangxx = $candidate; break } }
    if ($clangxx) { try { $res = & $clangxx -print-resource-dir 2>$null; if ($res) { $LINUX_CLANG_RESOURCE_INCLUDE_DIR = Join-Path $res "include" } } catch {} }

    # Gather system include dirs via g++ -v
    $LINUX_SYSTEM_INCLUDE_DIRS = @()
    if (Test-Command "g++") {
        try {
            # In PowerShell '< $null' is a reserved token. Pipe an empty string to g++ to avoid
            # using shell-style redirection which is invalid in PowerShell.
            $gppOut = "" | & g++ -E -x c++ - -v 2>&1
            $collect = $false
            foreach ($line in $gppOut) {
                if ($line -match '#include <...> search starts here:') { $collect = $true; continue }
                if ($line -match 'End of search list\.') { $collect = $false; break }
                if ($collect) { $dir = $line.Trim(); if ($dir -ne '' -and $dir -notmatch '/lib/gcc/.*/include$') { $LINUX_SYSTEM_INCLUDE_DIRS += $dir } }
            }
        }
        catch {}
    }

    $commonTidyArgs += "--extra-arg-before=--target=x86_64-linux-gnu"
    $commonTidyArgs += "--extra-arg-before=--driver-mode=g++"
    if ($LINUX_GCC_TOOLCHAIN_DIR) { $commonTidyArgs += "--extra-arg-before=--gcc-toolchain=$LINUX_GCC_TOOLCHAIN_DIR" }
    $commonTidyArgs += "--extra-arg=-I$LINT_INCLUDE_DIR"
    $commonTidyArgs += "--extra-arg=-isystem"
    $commonTidyArgs += "--extra-arg=$ROOT_DIR/tests/support"
    $commonTidyArgs += "--extra-arg=-fopenmp"
    $commonTidyArgs += "--extra-arg=-w"
    $commonTidyArgs += "--extra-arg=-fconstexpr-steps=4194304"
    $commonTidyArgs += "--extra-arg=-D__malloc__(...)=__malloc__"
    if ($LINUX_CLANG_RESOURCE_INCLUDE_DIR) { $commonTidyArgs += "--extra-arg-before=-isystem"; $commonTidyArgs += "--extra-arg-before=$LINUX_CLANG_RESOURCE_INCLUDE_DIR" }
    foreach ($inc in $LINUX_SYSTEM_INCLUDE_DIRS) { $commonTidyArgs += "--extra-arg-before=-isystem"; $commonTidyArgs += "--extra-arg-before=$inc" }
}

function Write-FilteredLintOutput {
    param ([object[]]$lines)

    $filterPattern = '^\d+ warnings? generated\.$|^\d+ warnings? and \d+ errors? generated\.$|^\[\d+/\d+\] Processing file'
    foreach ($line in $lines) {
        $text = $line.ToString()
        if ($text -and $text -notmatch $filterPattern) {
            Write-Host $text
        }
    }
}

function Invoke-ParallelWorkItems {
    param (
        [object[]]$WorkItems,
        [int]$ThrottleLimit,
        [scriptblock]$JobScript,
        [scriptblock]$ResultHandler
    )

    if ($null -eq $WorkItems -or $WorkItems.Count -eq 0) {
        return
    }

    if ($ThrottleLimit -lt 1) {
        $ThrottleLimit = 1
    }

    $hadFailure = $false
    for ($offset = 0; $offset -lt $WorkItems.Count; $offset += $ThrottleLimit) {
        $jobs = @()
        $batchEnd = [Math]::Min($offset + $ThrottleLimit, $WorkItems.Count)
        for ($index = $offset; $index -lt $batchEnd; $index++) {
            $jobs += Start-Job -ScriptBlock $JobScript -ArgumentList $WorkItems[$index]
        }

        Wait-Job -Job $jobs | Out-Null

        foreach ($job in $jobs) {
            $result = Receive-Job -Job $job
            & $ResultHandler $result
            if ($result.ExitCode -ne 0) {
                $hadFailure = $true
            }
            Remove-Job -Job $job -Force | Out-Null
        }
    }

    if ($hadFailure) {
        throw "One or more lint jobs failed."
    }
}

$clangTidyJobScript = {
    param($workItem)

    try {
        $output = & $workItem.Command @($workItem.Arguments) 2>&1 | ForEach-Object { $_.ToString() }
        $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    }
    catch {
        $output = @($_.Exception.Message)
        $exitCode = 1
    }

    [pscustomobject]@{
        Label    = $workItem.Label
        ExitCode = $exitCode
        Output   = @($output)
    }
}

$pythonLintJobScript = {
    param($workItem)

    try {
        $output = & $workItem.Command @($workItem.Arguments) 2>&1 | ForEach-Object { $_.ToString() }
        $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    }
    catch {
        $output = @($_.Exception.Message)
        $exitCode = 1
    }

    [pscustomobject]@{
        Label    = $workItem.Label
        ExitCode = $exitCode
        Output   = @($output)
    }
}

$lintJobs = Get-ParallelJobCount
Write-Host "Using up to $lintJobs parallel lint jobs." -ForegroundColor Gray

# Collect files from args or default locations (do not lint tests/ by default)
$lintTargets = if ($args) { $args } else {
    @(
        (Join-Path $ROOT_DIR "src"),
        (Join-Path $ROOT_DIR "vendor")
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

        $tidyDbDir = Join-Path $BUILD_DIR "tidy_db"
        if (-not (Test-Path $tidyDbDir)) { New-Item -ItemType Directory -Path $tidyDbDir -Force | Out-Null }

        $sanitizerPath = Join-Path $env:TEMP "sanitize_compile_commands_for_tidy.py"
        $pythonScript = @'
import ctypes
import json
import pathlib
import shlex
import sys


def _is_pch_path(arg: str) -> bool:
    lower = arg.lower()
    return "cmake_pch.h" in lower or "cmake_pch.hxx" in lower


def _sanitize_args(args):
    sanitized = []
    i = 0
    while i < len(args):
        arg = args[i]

        if arg in ("-include", "-include-pch") and i + 1 < len(args):
            if _is_pch_path(args[i + 1]):
                i += 2
                continue

        # Clang sometimes emits PCH args as:
        #   -Xclang -include-pch -Xclang /path/to/cmake_pch.hxx.pch
        if arg == "-Xclang" and i + 1 < len(args):
            next_arg = args[i + 1]
            if next_arg in ("-include", "-include-pch"):
                if i + 3 < len(args) and args[i + 2] == "-Xclang" and _is_pch_path(args[i + 3]):
                    i += 4
                    continue
                i += 2
                continue
            if _is_pch_path(next_arg):
                i += 2
                continue

        if arg.startswith("-include-pch") and _is_pch_path(arg):
            i += 1
            continue

        if arg.startswith("-include") and _is_pch_path(arg):
            i += 1
            continue

        if _is_pch_path(arg):
            i += 1
            continue

        sanitized.append(arg)
        i += 1
    return sanitized


def _split_command(command: str) -> list:
    # shlex.split uses POSIX rules and eats backslashes in Windows paths.
    # On Windows, use CommandLineToArgvW which matches how the compiler is
    # actually invoked (via CreateProcess, not a POSIX shell).
    if sys.platform == "win32":
        _fn = ctypes.windll.shell32.CommandLineToArgvW
        _fn.restype = ctypes.POINTER(ctypes.c_wchar_p)
        _fn.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
        argc = ctypes.c_int(0)
        argv_ptr = _fn(command, ctypes.byref(argc))
        if not argv_ptr:
            return shlex.split(command)
        try:
            return [argv_ptr[i] for i in range(argc.value)]
        finally:
            ctypes.windll.kernel32.LocalFree(argv_ptr)
    return shlex.split(command)


src_path = pathlib.Path(sys.argv[1])
dst_path = pathlib.Path(sys.argv[2])
entries = json.loads(src_path.read_text(encoding="utf-8"))

for entry in entries:
    if "arguments" in entry and isinstance(entry["arguments"], list):
        entry["arguments"] = _sanitize_args(entry["arguments"])
        continue

    command = entry.get("command")
    if isinstance(command, str) and command.strip():
        try:
            split = _split_command(command)
            sanitized = _sanitize_args(split)
            # Write as arguments array so clang-tidy receives tokens directly
            # without any further shell re-parsing or re-quoting.
            entry.pop("command", None)
            entry["arguments"] = sanitized
        except (ValueError, OSError):
            pass

dst_path.parent.mkdir(parents=True, exist_ok=True)
dst_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
'@

        Set-Content -Path $sanitizerPath -Value $pythonScript -Encoding UTF8
        Assert-Command "python"
        $targetPath = Join-Path $tidyDbDir "compile_commands.json"
        $pyResult = & python $sanitizerPath $COMPILE_COMMANDS $targetPath 2>&1
        if ($LASTEXITCODE -ne 0) { Write-Error "Failed to sanitize compile_commands.json via Python.`n$pyResult"; exit 1 }
        Remove-Item $sanitizerPath -ErrorAction SilentlyContinue

        $COMPILE_COMMANDS = $targetPath
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
        $workItems = foreach ($file in $compileDbFiles) {
            [pscustomobject]@{
                Label     = $file
                Command   = $clangTidyBin
                Arguments = $commonTidyArgs + @("-p", "$tidyDbDir") + $compileDbIncludeArgs + @($file)
            }
        }

        Invoke-ParallelWorkItems -WorkItems $workItems -ThrottleLimit $lintJobs -JobScript $clangTidyJobScript -ResultHandler {
            param($result)
            Write-Host "Linting $($result.Label)..." -ForegroundColor Gray
            Write-FilteredLintOutput $result.Output
        }
    }

    if ($standaloneFiles) {
        Write-Host "Linting $($standaloneFiles.Count) standalone files..." -ForegroundColor Cyan
        $workItems = foreach ($file in $standaloneFiles) {
            $includeArgs = @("-I$LINT_INCLUDE_DIR")
            foreach ($inc in $EXTRA_INCLUDES) {
                $includeArgs += "-I$inc"
            }
            $includeArgs += "-I$((Get-Item $file).DirectoryName)"

            [pscustomobject]@{
                Label     = $file
                Command   = $clangTidyBin
                Arguments = $commonTidyArgs + @($file, "--", "--driver-mode=g++", "-std=c++20", "-x", "c++") + $includeArgs
            }
        }

        Invoke-ParallelWorkItems -WorkItems $workItems -ThrottleLimit $lintJobs -JobScript $clangTidyJobScript -ResultHandler {
            param($result)
            Write-Host "Linting $($result.Label)..." -ForegroundColor Gray
            Write-FilteredLintOutput $result.Output
        }
    }
}

if ($pythonFiles) {
    Assert-Command "python"
    Write-Host "Linting $($pythonFiles.Count) Python files..." -ForegroundColor Cyan
    $workItems = foreach ($file in $pythonFiles) {
        [pscustomobject]@{
            Label     = $file
            Command   = "python"
            Arguments = @("-m", "py_compile", $file)
        }
    }

    Invoke-ParallelWorkItems -WorkItems $workItems -ThrottleLimit $lintJobs -JobScript $pythonLintJobScript -ResultHandler {
        param($result)
        Write-Host "Linting $($result.Label)..." -ForegroundColor Gray
        Write-FilteredLintOutput $result.Output
    }
}
