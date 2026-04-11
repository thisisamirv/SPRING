$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item "$SCRIPT_DIR\..").FullName
$ASSET_DIR = Join-Path $ROOT_DIR "assets\sample-data"
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe"

$SPRING_SMOKE_MODE = $env:SPRING_SMOKE_MODE
if (-not $SPRING_SMOKE_MODE) { $SPRING_SMOKE_MODE = "full" }

$SPRING_COMMAND_TIMEOUT_SECONDS = $env:SPRING_COMMAND_TIMEOUT_SECONDS
if (-not $SPRING_COMMAND_TIMEOUT_SECONDS) { $SPRING_COMMAND_TIMEOUT_SECONDS = 0 }

# Create a temporary working directory inside tests/output
$tempBase = Join-Path $ROOT_DIR "tests\output\smoke-test."
$uniqueId = [System.Guid]::NewGuid().ToString().Substring(0, 8)
$WORK_DIR = $tempBase + $uniqueId
New-Item -ItemType Directory -Path $WORK_DIR -Force | Out-Null

$CURRENT_SMOKE_CASE = ""

function Write-SmokeDebug {
    param ($exitCode)
    Write-Host "Smoke debug: case=$($global:CURRENT_SMOKE_CASE) exit_code=$exitCode" -ForegroundColor Yellow
    Write-Host "Smoke debug: pwd=$(Get-Location)" -ForegroundColor Yellow
    
    if (Test-Path ".") {
        Write-Host "Smoke debug: Directory listing:" -ForegroundColor Yellow
        Get-ChildItem -File | Select-Object Name, Length, LastWriteTime | ForEach-Object {
            Write-Host "Smoke debug: File: $($_.Name) Size: $($_.Length) Date: $($_.LastWriteTime)" -ForegroundColor Yellow
        }
    }

    $candidates = @("win-single", "win-paired", "win-gzip", "win-single-out", "win-paired-out.1", "win-paired-out.2", "win-gzip-out", "abcd", "tmp", "tmp.1", "tmp.2")
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            Write-Host "Smoke debug: found $candidate" -ForegroundColor Yellow
            Get-Item $candidate | Select-Object Name, Length, LastWriteTime | Out-String | Write-Host -ForegroundColor Yellow
        }
    }

    # Tar contents check (modern Windows has tar.exe)
    if (Get-Command "tar" -ErrorAction SilentlyContinue) {
        $archives = @("win-single", "win-paired", "win-gzip", "abcd")
        foreach ($archive in $archives) {
            if (Test-Path $archive -PathType Leaf) {
                Write-Host "Smoke debug: tar contents for $archive" -ForegroundColor Yellow
                try {
                    tar -tf $archive 2>&1 | Out-String | Write-Host -ForegroundColor Yellow
                } catch {
                    # Ignore tar errors
                }
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
    
    # Unroll arguments if they were passed as an array subexpression (e.g., Invoke-Spring @(...))
    # which causes them to be nested in $args[0].
    foreach ($arg in $args) {
        if ($arg -is [array]) {
            $fullArgs += $arg
        } else {
            $fullArgs += $arg
        }
    }

    if ($fullArgs.Count -eq 0) { return }

    $exe = $fullArgs[0]
    [string[]]$remainingArgs = $fullArgs[1..($fullArgs.Count - 1)]

    Write-Host "Running Spring command: $exe $([string]::Join(' ', $remainingArgs))" -ForegroundColor Cyan
    
    $exitCode = 0
    if ($SPRING_COMMAND_TIMEOUT_SECONDS -gt 0 -and (Get-Command "timeout" -ErrorAction SilentlyContinue)) {
        $process = Start-Process -FilePath $exe -ArgumentList $remainingArgs -Wait -NoNewWindow -PassThru
        $exitCode = $process.ExitCode
    }
    else {
        # Capture stderr to see what happened on failure
        $errFile = Join-Path $ROOT_DIR "spring_error.txt"
        & $exe @remainingArgs 2>$errFile
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            Write-Host "Spring Error Output:" -ForegroundColor Red
            if (Test-Path $errFile) {
                Get-Content $errFile | Write-Host -ForegroundColor Red
            }
        }
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

# Add C# helper for robust multi-member GZIP decompression (handles BGZF)
$GZipHelperSource = @"
using System;
using System.IO;
using System.IO.Compression;

public class GZipHelper {
    public static void Decompress(string infile, string outfile) {
        byte[] data = File.ReadAllBytes(infile);
        using (var outputStream = File.Create(outfile)) {
            for (int i = 0; i < data.Length - 1; i++) {
                if (data[i] == 0x1F && data[i+1] == 0x8B) {
                    using (var inputStream = new MemoryStream(data, i, data.Length - i))
                    using (var gz = new GZipStream(inputStream, CompressionMode.Decompress)) {
                        try {
                            gz.CopyTo(outputStream);
                        } catch {
                            // Skip invalid members or trailing data
                        }
                    }
                    // We don't know the compressed size, so we'll just scan for the next magic number.
                    // To avoid re-decompressing the same member, we should ideally skip ahead.
                    // But for a smoke test with small files, scanning is fast enough.
                    // Actually, GZipStream will have decompressed one member. 
                    // Any magic number inside that compressed member will be ignored because we're scanning the COMPRESSED data.
                }
            }
        }
    }
}
"@

try {
    Add-Type -TypeDefinition $GZipHelperSource -ErrorAction SilentlyContinue
} catch {}

function Expand-GzipFile {
    param($infile, $outfile)
    $infileFull = (Get-Item $infile).FullName
    $outfileFull = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $outfile))
    
    try {
        [GZipHelper]::Decompress($infileFull, $outfileFull)
    }
    catch {
        Write-Host "Note: Gzip decompression failed: $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Compare-Files {
    param ($leftPath, $rightPath)
    
    if (-not (Test-Path $leftPath)) {
        Write-Host "Left comparison file does not exist: $leftPath" -ForegroundColor Red
        return $false
    }
    if (-not (Test-Path $rightPath)) {
        Write-Host "Right comparison file does not exist: $rightPath" -ForegroundColor Red
        return $false
    }

    # Normalize line endings (strip CR) for robust comparison on Windows
    $bytes1 = [System.IO.File]::ReadAllBytes((Get-Item $leftPath).FullName)
    $bytes2 = [System.IO.File]::ReadAllBytes((Get-Item $rightPath).FullName)
    
    $clean1 = [System.Linq.Enumerable]::ToArray([System.Linq.Enumerable]::Where($bytes1, [Func[byte,bool]]{ param($b) $b -ne 0x0D }))
    $clean2 = [System.Linq.Enumerable]::ToArray([System.Linq.Enumerable]::Where($bytes2, [Func[byte,bool]]{ param($b) $b -ne 0x0D }))

    $ms1 = New-Object System.IO.MemoryStream($clean1, $false)
    $ms2 = New-Object System.IO.MemoryStream($clean2, $false)
    
    $hash1 = (Get-FileHash -InputStream $ms1).Hash
    $hash2 = (Get-FileHash -InputStream $ms2).Hash
    
    $ms1.Dispose()
    $ms2.Dispose()

    if ($hash1 -ne $hash2) {
        Write-Host "Files differ in hash (normalized): $leftPath ($($clean1.Length) bytes) vs $rightPath ($($clean2.Length) bytes)" -ForegroundColor Red
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
        Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -o abcd
        Invoke-Spring -d -i abcd -o tmp
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-SmokeCase "single fasta round-trip"
        Invoke-Spring -c -i "$ASSET_DIR\test_1.fasta" -o abcd
        Invoke-Spring -d -i abcd -o tmp
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

        Write-SmokeCase "paired fastq round-trip"
        Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd
        Invoke-Spring -d -i abcd -o tmp
        if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
        if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

        Write-SmokeCase "gzipped fastq input round-trip"
        Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq.gz" -o abcd
        Invoke-Spring -d -i abcd -o tmp
        if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-Host "Tests successful!" -ForegroundColor Green
        exit 0
    }

    if ($SPRING_SMOKE_MODE -eq "windows-quick") {
        Write-SmokeCase "single fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq" "win-single-input.fastq"
        Invoke-Spring -c -i win-single-input.fastq -o win-single
        Invoke-Spring -d -i win-single -o win-single-out
        if (-not (Compare-Files "win-single-out" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-SmokeCase "paired fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq" "win-paired-input-1.fastq"
        Initialize-SmokeInput "$ASSET_DIR\test_2.fastq" "win-paired-input-2.fastq"
        Invoke-Spring -c -i win-paired-input-1.fastq win-paired-input-2.fastq -o win-paired
        Invoke-Spring -d -i win-paired -o win-paired-out
        if (-not (Compare-Files "win-paired-out.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
        if (-not (Compare-Files "win-paired-out.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

        Write-SmokeCase "gzipped fastq long-mode round-trip"
        Initialize-SmokeInput "$ASSET_DIR\test_1.fastq.gz" "win-gzip-input.fastq.gz"
        Invoke-Spring -c -i win-gzip-input.fastq.gz -o win-gzip
        Invoke-Spring -d -i win-gzip -o win-gzip-out
        if (-not (Compare-Files "win-gzip-out" "$ASSET_DIR\test_1.fastq")) { exit 1 }

        Write-Host "Tests successful!" -ForegroundColor Green
        exit 0
    }

    # Full smoke test mode
    Write-SmokeCase "fastq round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "fasta round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fasta" "$ASSET_DIR\test_2.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "fastq to gzipped output"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    Invoke-Spring -d -i abcd -o tmp.gz
    Expand-GzipFile "tmp.gz" "tmp.decompressed"
    if (-not (Compare-Files "tmp.decompressed" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "fasta round-trip repeat"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq round-trip repeat"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq.gz" "$ASSET_DIR\test_2.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta gzipped input round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fasta.gz" "$ASSET_DIR\test_2.fasta.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "single gzipped fastq round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "single gzipped fastq to gzipped output"
    Invoke-Spring -d -i abcd -o tmp.gz
    Expand-GzipFile "tmp.gz" "tmp.decompressed"
    if (-not (Compare-Files "tmp.decompressed" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input round-trip redundant"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq.gz" "$ASSET_DIR\test_2.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input to gzipped outputs"
    Invoke-Spring -d -i abcd -o tmp.1.gz tmp.2.gz
    Expand-GzipFile "tmp.1.gz" "tmp.1"
    Expand-GzipFile "tmp.2.gz" "tmp.2"
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "multi-threaded round-trip single"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -o abcd -t 8
    Invoke-Spring -d -i abcd -o tmp -t 5
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "multi-threaded round-trip paired"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd -t 8
    Invoke-Spring -d -i abcd -o tmp -t 5
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "sorted output round-trip single"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -o abcd -s o
    Invoke-Spring -d -i abcd -o tmp
    Get-Content "tmp" | Sort-Object | Set-Content "tmp.sorted"
    Get-Content "$ASSET_DIR\test_1.fastq" | Sort-Object | Set-Content "tmp_1.sorted"
    if (-not (Compare-Files "tmp.sorted" "tmp_1.sorted")) { exit 1 }

    Write-SmokeCase "sorted output round-trip paired"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd -t 8
    Invoke-Spring -d -i abcd -o tmp -t 5
    Get-Content "tmp.1" | Sort-Object | Set-Content "tmp.1.sorted"
    Get-Content "$ASSET_DIR\test_1.fastq" | Sort-Object | Set-Content "tmp_1.sorted"
    if (-not (Compare-Files "tmp.1.sorted" "tmp_1.sorted")) { exit 1 }
    Get-Content "tmp.2" | Sort-Object | Set-Content "tmp.2.sorted"
    Get-Content "$ASSET_DIR\test_2.fastq" | Sort-Object | Set-Content "tmp_2.sorted"
    if (-not (Compare-Files "tmp.2.sorted" "tmp_2.sorted")) { exit 1 }

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
