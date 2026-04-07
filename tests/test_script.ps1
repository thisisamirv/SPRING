$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item "$SCRIPT_DIR\..").FullName
$ASSET_DIR = Join-Path $SCRIPT_DIR "assets"
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe"

$SPRING_SMOKE_MODE = $env:SPRING_SMOKE_MODE
if (-not $SPRING_SMOKE_MODE) { $SPRING_SMOKE_MODE = "full" }

$SPRING_COMMAND_TIMEOUT_SECONDS = $env:SPRING_COMMAND_TIMEOUT_SECONDS
if (-not $SPRING_COMMAND_TIMEOUT_SECONDS) { $SPRING_COMMAND_TIMEOUT_SECONDS = 0 }

# Create a temporary working directory
$tempBase = Join-Path $BUILD_DIR "smoke-test."
$uniqueId = [System.Guid]::NewGuid().ToString().Substring(0, 8)
$WORK_DIR = $tempBase + $uniqueId
New-Item -ItemType Directory -Path $WORK_DIR -Force | Out-Null

$CURRENT_SMOKE_CASE = ""

function Write-SmokeDebug {
    param ($exitCode)
    Write-Error "Smoke debug: case=$($global:CURRENT_SMOKE_CASE) exit_code=$exitCode"
    Write-Error "Smoke debug: pwd=$(Get-Location)"
    
    if (Test-Path ".") {
        Get-ChildItem -Force | Out-String | Write-Error
    }

    $candidates = @("win-single", "win-paired", "win-gzip", "win-single-out", "win-paired-out.1", "win-paired-out.2", "win-gzip-out", "abcd", "tmp", "tmp.1", "tmp.2")
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            Write-Error "Smoke debug: found $candidate"
            Get-Item $candidate | Select-Object Name, Length, LastWriteTime | Out-String | Write-Error
        }
    }

    # Tar contents check (modern Windows has tar.exe)
    if (Get-Command "tar" -ErrorAction SilentlyContinue) {
        $archives = @("win-single", "win-paired", "win-gzip", "abcd")
        foreach ($archive in $archives) {
            if (Test-Path $archive -PathType Leaf) {
                Write-Error "Smoke debug: tar contents for $archive"
                try {
                    tar -tf $archive 2>&1 | Out-String | Write-Error
                }
                catch {}
            }
        }
    }
}

function Remove-SmokeWorkDir {
    if (Test-Path $WORK_DIR) {
        Remove-Item -Recurse -Force $WORK_DIR -ErrorAction SilentlyContinue
    }
}

function Invoke-Spring {
    $fullArgs = @()
    if ($env:SPRING_BIN_WRAPPER) {
        $fullArgs += $env:SPRING_BIN_WRAPPER.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)
    }
    
    $fullArgs += $SPRING_BIN
    
    if ($env:SPRING_TEST_ARGS) {
        $fullArgs += $env:SPRING_TEST_ARGS.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)
    }
    
    # Append any remaining arguments passed to Run-Spring
    $fullArgs += $args

    if ($fullArgs.Count -eq 0) { return }

    $exe = $fullArgs[0]
    [string[]]$remainingArgs = $fullArgs[1..($fullArgs.Count - 1)]

    Write-Host "Running Spring command: $exe $([string]::Join(' ', $remainingArgs))" -ForegroundColor Cyan
    
    if ($SPRING_COMMAND_TIMEOUT_SECONDS -gt 0 -and (Get-Command "timeout" -ErrorAction SilentlyContinue)) {
        $process = Start-Process -FilePath $exe -ArgumentList $remainingArgs -Wait -NoNewWindow -PassThru
        $exitCode = $process.ExitCode
    }
    else {
        & $exe @remainingArgs
        $exitCode = $LASTEXITCODE
    }

    if ($exitCode -ne 0) {
        $global:LAST_SPRING_EXIT_CODE = $exitCode
        throw "Spring failed with exit code $exitCode"
    }
}

function Write-SmokeCase {
    param ($caseName)
    $global:CURRENT_SMOKE_CASE = $caseName
    Write-Host "Smoke case: $caseName" -ForegroundColor Cyan
}

function Compare-Files {
    param ($leftPath, $rightPath)
    
    if (-not (Test-Path $leftPath) -or -not (Test-Path $rightPath)) {
        throw "One of the files to compare does not exist: $leftPath, $rightPath"
    }

    $leftHash = Get-FileHash $leftPath -Algorithm SHA256
    $rightHash = Get-FileHash $rightPath -Algorithm SHA256

    if ($leftHash.Hash -ne $rightHash.Hash) {
        Write-Error "Files differ: $leftPath $rightPath"
        return $false
    }
    return $true
}

function Initialize-SmokeInput {
    param ($sourcePath, $targetName)
    Copy-Item $sourcePath $targetName -Force
}

# Equivalent of 'trap' in Bash
try {
    if (-not (Test-Path $SPRING_BIN)) {
        Write-Error "Expected built binary at $SPRING_BIN"
        exit 1
    }

    Push-Location $WORK_DIR

    if ($SPRING_SMOKE_MODE -eq "quick") {
        Write-SmokeCase "single fastq round-trip"
        Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "-o", "abcd")
        Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-SmokeCase "single fasta round-trip"
        Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fasta", "-o", "abcd")
        Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

        Write-SmokeCase "paired fastq round-trip"
        Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "$ASSET_DIR\test_2.fastq", "-o", "abcd")
        Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
        if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
        if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

        Write-SmokeCase "gzipped fastq input round-trip"
        Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq.gz", "-o", "abcd")
        Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-Host "Tests successful!" -ForegroundColor Green
        exit 0
    }

    if ($SPRING_SMOKE_MODE -eq "windows-quick") {
        Write-SmokeCase "single fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq" "win-single-input.fastq"
        Invoke-Spring @("-c", "-i", "win-single-input.fastq", "-o", "win-single")
        Invoke-Spring @("-d", "-i", "win-single", "-o", "win-single-out")
        if (-not (Compare-Files "win-single-out" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-SmokeCase "paired fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq" "win-paired-input-1.fastq"
        Initialize-SmokeInput "$ASSET_DIR\test_2.fastq" "win-paired-input-2.fastq"
        Invoke-Spring @("-c", "-i", "win-paired-input-1.fastq", "win-paired-input-2.fastq", "-o", "win-paired")
        Invoke-Spring @("-d", "-i", "win-paired", "-o", "win-paired-out")
        if (-not (Compare-Files "win-paired-out.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
        if (-not (Compare-Files "win-paired-out.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

        Write-SmokeCase "gzipped fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq.gz" "win-gzip-input.fastq.gz"
        Invoke-Spring @("-c", "-i", "win-gzip-input.fastq.gz", "-o", "win-gzip")
        Invoke-Spring @("-d", "-i", "win-gzip", "-o", "win-gzip-out")
        if (-not (Compare-Files "win-gzip-out" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-Host "Tests successful!" -ForegroundColor Green
        exit 0
    }

    # Full smoke test mode
    Write-SmokeCase "fastq round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "fasta round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fasta", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "$ASSET_DIR\test_2.fastq", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fasta", "$ASSET_DIR\test_2.fasta", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "fastq to gzipped output"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp.gz")
    if (Get-Command "tar" -ErrorAction SilentlyContinue) {
        tar -xf tmp.gz
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    }

    Write-SmokeCase "fasta round-trip repeat"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fasta", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq.gz", "$ASSET_DIR\test_2.fastq.gz", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta gzipped input round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fasta.gz", "$ASSET_DIR\test_2.fasta.gz", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "single gzipped fastq round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq.gz", "-o", "abcd")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "multi-threaded round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "-o", "abcd", "-t", "8")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp", "-t", "5")
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "sorted output round-trip"
    Invoke-Spring @("-c", "-i", "$ASSET_DIR\test_1.fastq", "-o", "abcd", "-s", "o")
    Invoke-Spring @("-d", "-i", "abcd", "-o", "tmp")
    Get-Content "tmp" | Sort-Object | Set-Content "tmp.sorted"
    Get-Content "$ASSET_DIR\test_1.fastq" | Sort-Object | Set-Content "tmp_1.sorted"
    if (-not (Compare-Files "tmp.sorted" "tmp_1.sorted")) { exit 1 }

    Write-Host "Tests successful!" -ForegroundColor Green

}
catch {
    Write-SmokeDebug $LASTEXITCODE
    throw $_
}
finally {
    Pop-Location
    Remove-SmokeWorkDir
}
