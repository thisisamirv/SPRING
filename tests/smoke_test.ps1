$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item "$SCRIPT_DIR\..").FullName
$ASSET_DIR = Join-Path $ROOT_DIR "assets\sample-data"
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe"
$SPRING_PREVIEW_BIN = Join-Path $BUILD_DIR "spring2-preview.exe"

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
                }
                catch {
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
    param(
        [string]$BinaryPath = $global:SPRING_BIN
    )
    $fullArgs = @()
    if ($env:SPRING_BIN_WRAPPER) {
        $fullArgs += $env:SPRING_BIN_WRAPPER.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)
    }

    $fullArgs += $BinaryPath

    if ($env:SPRING_TEST_ARGS) {
        $fullArgs += $env:SPRING_TEST_ARGS.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)
    }

    if ($args) {
        $fullArgs += $args
    }

    if ($fullArgs.Count -eq 0) { return }

    $exe = $fullArgs[0]
    [string[]]$cmdArgs = $fullArgs[1..($fullArgs.Count - 1)]

    $exitCode = 0
    if ($SPRING_COMMAND_TIMEOUT_SECONDS -gt 0 -and (Get-Command "timeout" -ErrorAction SilentlyContinue)) {
        $process = Start-Process -FilePath $exe -ArgumentList $cmdArgs -Wait -NoNewWindow -PassThru
        $exitCode = $process.ExitCode
    }
    else {
        & $exe @cmdArgs
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

# Add C# helper for robust multi-member GZIP decompression (handles BGZF)
$GZipHelperSource = @"
using System;
using System.IO;
using System.IO.Compression;

public class GZipHelper {
    public static void Decompress(string infile, string outfile) {
        byte[] data = File.ReadAllBytes(infile);
        using (var outputStream = File.Create(outfile)) {
            int i = 0;
            while (i < data.Length - 1) {
                if (data[i] == 0x1F && data[i+1] == 0x8B) {
                    int start = i;
                    // Check if BGZF (has BSIZ in extra field)
                    int blocksize = -1;
                    if (data.Length - i >= 20 && (data[i+3] & 0x04) != 0) {
                        // Scan for BC/02 subfield
                        int xlen = data[i+10] | (data[i+11] << 8);
                        int p = i + 12;
                        while (p < i + 12 + xlen - 4) {
                            if (data[p] == 0x42 && data[p+1] == 0x43 && data[p+2] == 0x02 && data[p+3] == 0x00) {
                                blocksize = (data[p+4] | (data[p+5] << 8)) + 1;
                                break;
                            }
                            p += (data[p+2] | (data[p+3] << 8)) + 4;
                        }
                    }

                    using (var inputStream = new MemoryStream(data, i, data.Length - i))
                    using (var gz = new GZipStream(inputStream, CompressionMode.Decompress)) {
                        try {
                            gz.CopyTo(outputStream);
                        } catch {
                            // If it fails, maybe not a real gzip member or corrupted
                        }
                    }

                    if (blocksize > 0) {
                        i += blocksize;
                    } else {
                        // For non-BGZF files, we assume a single member for testing purposes.
                        // Blind scanning for 0x1F 0x8B magic numbers inside compressed payloads
                        // leads to false-positive matches and data corruption.
                        break;
                    }
                } else {
                    i++;
                }
            }
        }
    }
}
"@

try {
    Add-Type -TypeDefinition $GZipHelperSource -ErrorAction SilentlyContinue
}
catch {}

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

    $clean1 = [System.Linq.Enumerable]::ToArray([System.Linq.Enumerable]::Where($bytes1, [Func[byte, bool]] { param($b) $b -ne 0x0D }))
    $clean2 = [System.Linq.Enumerable]::ToArray([System.Linq.Enumerable]::Where($bytes2, [Func[byte, bool]] { param($b) $b -ne 0x0D }))

    $ms1 = New-Object System.IO.MemoryStream($clean1, $false)
    $ms2 = New-Object System.IO.MemoryStream($clean2, $false)

    $hash1 = (Get-FileHash -InputStream $ms1).Hash
    $hash2 = (Get-FileHash -InputStream $ms2).Hash

    $ms1.Dispose()
    $ms2.Dispose()

    if ($hash1 -ne $hash2) {
        Write-Host "Files differ in hash (normalized): $leftPath ($($clean1.Length) bytes) vs $rightPath ($($clean2.Length) bytes)" -ForegroundColor Red

        # Diagnostic: show line counts
        $lines1 = Get-Content $leftPath
        $lines2 = Get-Content $rightPath
        Write-Host "Lines: $($lines1.Count) vs $($lines2.Count)" -ForegroundColor Yellow

        # Diagnostic: show diff
        Write-Host "Showing first 10 differences (Compare-Object):" -ForegroundColor Yellow
        Compare-Object $lines1 $lines2 | Select-Object -First 10 | Out-String | Write-Host -ForegroundColor Yellow

        return $false
    }
    return $true
}

function Compare-Lines {
    param ($leftPath, $rightPath)
    if (-not (Test-Path $leftPath)) { return $false }
    if (-not (Test-Path $rightPath)) { return $false }

    $l1 = (Get-Content $leftPath | Measure-Object -Line).Lines
    $l2 = (Get-Content $rightPath | Measure-Object -Line).Lines

    if ($l1 -ne $l2) {
        Write-Host "Line count differ: $leftPath ($l1) vs $rightPath ($l2)" -ForegroundColor Red
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
    $ref1 = Get-Content "$ASSET_DIR\test_1.fastq" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $act1 = Get-Content "tmp" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $ref1 | Set-Content "ref.sorted"
    $act1 | Set-Content "tmp.sorted"
    if (-not (Compare-Files "tmp.sorted" "ref.sorted")) { exit 1 }

    Write-SmokeCase "sorted output round-trip paired"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" "$ASSET_DIR\test_2.fastq" -o abcd -s o -t 8
    Invoke-Spring -d -i abcd -o tmp -t 5

    $ref1 = Get-Content "$ASSET_DIR\test_1.fastq" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $act1 = Get-Content "tmp.1" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $ref1 | Set-Content "ref.1.sorted"
    $act1 | Set-Content "tmp.1.sorted"
    if (-not (Compare-Files "tmp.1.sorted" "ref.1.sorted")) { exit 1 }

    $ref2 = Get-Content "$ASSET_DIR\test_2.fastq" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $act2 = Get-Content "tmp.2" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $ref2 | Set-Content "ref.2.sorted"
    $act2 | Set-Content "tmp.2.sorted"
    if (-not (Compare-Files "tmp.2.sorted" "ref.2.sorted")) { exit 1 }

    # Long-read mode test
    Write-SmokeCase "long-read mode round-trip"
    Invoke-Spring -c -i "$ASSET_DIR\sample.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\sample.fastq")) { exit 1 }

    # Memory capping test
    Write-SmokeCase "memory capping test"
    Invoke-Spring -m 0.1 -c -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -m 0.1 -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Archive Notes & Previewer Test
    Write-SmokeCase "archive notes & previewer validation"
    Invoke-Spring -c -i "$ASSET_DIR\test_1.fastq" -n "SMOKE_TEST_NOTE" -o abcd
    $previewOut = "preview.out"
    Invoke-Spring -BinaryPath $SPRING_PREVIEW_BIN abcd | Out-File $previewOut
    $previewContent = Get-Content $previewOut -Raw
    if ($previewContent -notmatch "SMOKE_TEST_NOTE") {
        Write-Error "Failed to find custom note in preview tool output"
    }
    if ($previewContent -notmatch "test_1\.fastq") {
        Write-Error "Failed to find original filename in preview tool output"
    }

    # Lossy quality mode: ill_bin
    Write-SmokeCase "lossy mode: ill_bin"
    Invoke-Spring -c -q ill_bin -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Lossy quality mode: qvz
    Write-SmokeCase "lossy mode: qvz"
    Invoke-Spring -c -q qvz 1 -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Lossy quality mode: binary thresholding
    Write-SmokeCase "lossy mode: binary"
    Invoke-Spring -c -q binary 30 40 10 -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Data Stripping: IDs
    Write-SmokeCase "stripping: ids"
    Invoke-Spring -c -s i -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Data Stripping: Quality
    Write-SmokeCase "stripping: quality"
    Invoke-Spring -c -s q -i "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

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
