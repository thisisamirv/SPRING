# scripts/common/common.ps1 - Shared PowerShell utility functions for SPRING2 development
$ErrorActionPreference = 'Stop'

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..\..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "out\build"
$SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe"
$COMPILE_COMMANDS = Join-Path $ROOT_DIR "out\clangd\compile_commands.json"

$DEFAULT_CPP_ROOTS = @(
    (Join-Path $ROOT_DIR "src"),
    (Join-Path $ROOT_DIR "experimental"),
    (Join-Path $ROOT_DIR "tests")
)

$DEFAULT_PY_ROOTS = @(
    (Join-Path $ROOT_DIR "experimental"),
    (Join-Path $ROOT_DIR "tests"),
    (Join-Path $ROOT_DIR "scripts")
)

$global:CompileCommandsFileSet = $null

function NormalizeCompileDbPath {
    param ([string]$path)
    if ([string]::IsNullOrWhiteSpace($path)) {
        return ""
    }
    return [System.IO.Path]::GetFullPath($path).Replace("\", "/").ToLowerInvariant()
}

function Initialize-CompileCommandsFileSet {
    if ($null -ne $global:CompileCommandsFileSet) {
        return
    }

    Assert-CompileCommands

    $global:CompileCommandsFileSet = [System.Collections.Generic.HashSet[string]]::new()
    $entries = Get-Content -Path $COMPILE_COMMANDS -Raw | ConvertFrom-Json

    foreach ($entry in $entries) {
        $entryFile = [string]$entry.file
        $entryDir = [string]$entry.directory
        if ([string]::IsNullOrWhiteSpace($entryFile)) {
            continue
        }

        $resolvedPath = if ([System.IO.Path]::IsPathRooted($entryFile)) {
            $entryFile
        }
        elseif (-not [string]::IsNullOrWhiteSpace($entryDir)) {
            Join-Path $entryDir $entryFile
        }
        else {
            Join-Path $ROOT_DIR $entryFile
        }

        [void]$global:CompileCommandsFileSet.Add((NormalizeCompileDbPath $resolvedPath))
    }
}

function Test-Command {
    param ($commandName)
    try {
        $found = Get-Command $commandName -ErrorAction SilentlyContinue
        return $null -ne $found
    }
    catch {
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

function Get-ParallelJobCount {
    $requestedJobs = $env:SPRING_LINT_JOBS
    if (-not [string]::IsNullOrWhiteSpace($requestedJobs)) {
        $jobCount = 0
        if (-not [int]::TryParse($requestedJobs, [ref]$jobCount) -or $jobCount -lt 1) {
            Write-Error "SPRING_LINT_JOBS must be a positive integer"
            exit 1
        }

        return $jobCount
    }

    $processorCount = [System.Environment]::ProcessorCount
    if ($processorCount -lt 1) {
        return 1
    }

    if ($processorCount -gt 1) {
        return ($processorCount - 1)
    }

    return $processorCount
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


function Get-CppSources {
    param ([string[]]$targetPaths)
    $isExplicit = ($null -ne $targetPaths -and $targetPaths.Count -gt 0)
    $searchPaths = if ($isExplicit) {
        $targetPaths | ForEach-Object { Resolve-RepoPath $_ } | Where-Object { Test-Path $_ }
    }
    else { $DEFAULT_CPP_ROOTS }

    # When the user explicitly provides target paths, also consider header files
    $includeList = if ($isExplicit) {
        @("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hpp", "*.hh", "*.hxx", "*.inl")
    }
    else {
        @("*.c", "*.cc", "*.cpp", "*.cxx")
    }

    $allowedExts = if ($isExplicit) {
        @('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hh', '.hxx', '.inl')
    }
    else {
        @('.c', '.cc', '.cpp', '.cxx')
    }

    $results = @()
    foreach ($p in $searchPaths) {
        if (Test-Path $p -PathType Container) {
            $results += Get-ChildItem -Path $p -Recurse -File -Include $includeList | Select-Object -ExpandProperty FullName | Sort-Object
        }
        elseif (Test-Path $p -PathType Leaf) {
            $ext = [System.IO.Path]::GetExtension($p).ToLowerInvariant()
            if ($allowedExts -contains $ext) { $results += (Get-Item $p).FullName }
        }
    }
    return $results
}

function Get-PythonSources {
    param ([string[]]$targetPaths)
    $searchPaths = if ($targetPaths) {
        $targetPaths | ForEach-Object { Resolve-RepoPath $_ } | Where-Object { Test-Path $_ }
    }
    else { $DEFAULT_PY_ROOTS }
    $results = @()
    foreach ($p in $searchPaths) {
        if (Test-Path $p -PathType Container) {
            $results += Get-ChildItem -Path $p -Recurse -File -Filter *.py | Select-Object -ExpandProperty FullName | Sort-Object
        }
        elseif (Test-Path $p -PathType Leaf) {
            if (Test-PythonSource $p) { $results += $p }
        }
    }
    return $results
}

function Test-CompileCommandsContains {
    param ($filePath)
    Initialize-CompileCommandsFileSet
    $normalizedTarget = NormalizeCompileDbPath $filePath
    return $global:CompileCommandsFileSet.Contains($normalizedTarget)
}
