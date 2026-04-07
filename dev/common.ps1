# dev/common.ps1 - Shared PowerShell utility functions for SPRING2 development
$ErrorActionPreference = 'Stop'

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe"
$COMPILE_COMMANDS = Join-Path $BUILD_DIR "compile_commands.json"

$DEFAULT_CPP_ROOTS = @(
    (Join-Path $ROOT_DIR "src"),
    (Join-Path $ROOT_DIR "experimental"),
    (Join-Path $ROOT_DIR "tests")
)

$DEFAULT_PY_ROOTS = @(
    (Join-Path $ROOT_DIR "experimental"),
    (Join-Path $ROOT_DIR "tests"),
    (Join-Path $ROOT_DIR "dev")
)

$VENDOR_ROOT = Join-Path $ROOT_DIR "vendor"

function Test-Command {
    param ($commandName)
    try {
        $found = Get-Command $commandName -ErrorAction SilentlyContinue
        return $null -ne $found
    } catch {
        return $false
    }
}

function Assert-Command {
    param ($commandName)
    if (-not (Test-Command $commandName)) {
        Write-Error "Missing required command: $commandName"
        exit 1
    }
}

function Assert-BuildDir {
    if (-not (Test-Path $BUILD_DIR)) {
        Write-Error "Expected build directory at $BUILD_DIR"
        Write-Error "Configure SPRING2 first, for example: cmake -S $ROOT_DIR -B $BUILD_DIR"
        exit 1
    }
}

function Assert-CompileCommands {
    Assert-BuildDir
    if (-not (Test-Path $COMPILE_COMMANDS)) {
        Write-Error "Expected compilation database at $COMPILE_COMMANDS"
        Write-Error "Re-run CMake so clang-tidy can use the generated compile commands."
        exit 1
    }
}

function Assert-SpringBinary {
    if (-not (Test-Path $SPRING_BIN)) {
        Write-Error "Expected built binary at $SPRING_BIN"
        Write-Error "Build SPRING2 first, for example: cmake --build $BUILD_DIR"
        exit 1
    }
}

function Test-CppSource {
    param ($filePath)
    return $filePath -match '\.(c|cc|cpp|cxx)$'
}

function Test-PythonSource {
    param ($filePath)
    return $filePath -match '\.py$'
}

function Resolve-RepoPath {
    param ($path)
    if ([System.IO.Path]::IsPathRooted($path)) {
        return (Get-Item $path).FullName
    }
    return (Join-Path $ROOT_DIR $path)
}

function Test-VendoredPath {
    param ($path)
    $resolvedPath = Resolve-RepoPath $path
    return $resolvedPath.StartsWith($VENDOR_ROOT, [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-FirstPartyPaths {
    param ([string[]]$paths)
    $results = @()
    foreach ($p in $paths) {
        $resolved = Resolve-RepoPath $p
        if (Test-VendoredPath $resolved) {
            Write-Host "Skipping vendored path: $resolved" -ForegroundColor Gray
            continue
        }
        if (Test-Path $resolved) {
            $results += $resolved
        }
    }
    return $results
}

function Get-CppSources {
    param ([string[]]$targetPaths)
    $searchPaths = if ($targetPaths) { Get-FirstPartyPaths $targetPaths } else { $DEFAULT_CPP_ROOTS }
    $results = @()
    foreach ($p in $searchPaths) {
        if (Test-Path $p -PathType Container) {
            $results += Get-ChildItem -Path $p -Recurse -File -Include *.c, *.cc, *.cpp, *.cxx | Select-Object -ExpandProperty FullName | Sort-Object
        } elseif (Test-Path $p -PathType Leaf) {
            if (Test-CppSource $p) { $results += $p }
        }
    }
    return $results
}

function Get-PythonSources {
    param ([string[]]$targetPaths)
    $searchPaths = if ($targetPaths) { Get-FirstPartyPaths $targetPaths } else { $DEFAULT_PY_ROOTS }
    $results = @()
    foreach ($p in $searchPaths) {
        if (Test-Path $p -PathType Container) {
            $results += Get-ChildItem -Path $p -Recurse -File -Filter *.py | Select-Object -ExpandProperty FullName | Sort-Object
        } elseif (Test-Path $p -PathType Leaf) {
            if (Test-PythonSource $p) { $results += $p }
        }
    }
    return $results
}

function Test-CompileCommandsContains {
    param ($filePath)
    # Use direct text search for performance, handling both forward and backslashes
    $normalized = $filePath.Replace("\", "/")
    $altNormalized = $filePath.Replace("/", "\")
    $searchString = """file"": ""$normalized"""
    $altSearchString = """file"": ""$altNormalized"""
    
    $found = Select-String -Path $COMPILE_COMMANDS -Pattern ([regex]::Escape($searchString)) -Quiet
    if ($found) { return $true }
    
    return Select-String -Path $COMPILE_COMMANDS -Pattern ([regex]::Escape($altSearchString)) -Quiet
}
