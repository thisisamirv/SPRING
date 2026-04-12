$ErrorActionPreference = 'Stop'

# Helper: GZip decompression for multi-member files (.gz/BGZF)
if (-not ("BigBencher" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.IO.Compression;

public class BigBencher {
    public static void Decompress(string inputPath, string outputPath) {
        using (FileStream output = File.Create(outputPath)) {
            using (FileStream input = File.OpenRead(inputPath)) {
                byte[] data = new byte[input.Length];
                input.Read(data, 0, (int)input.Length);
                MemoryStream result = new MemoryStream();
                
                for (int i = 0; i < data.Length - 10; i++) {
                    if (data[i] == 0x1f && data[i+1] == 0x8b && data[i+2] == 0x08) {
                        try {
                            using (MemoryStream memberMs = new MemoryStream(data, i, data.Length - i)) {
                                using (GZipStream decompressor = new GZipStream(memberMs, CompressionMode.Decompress)) {
                                    decompressor.CopyTo(result);
                                }
                            }
                        } catch { } // Skip junk
                    }
                }
                result.Position = 0;
                result.CopyTo(output);
            }
        }
    }

    public static Stream GetDecompressedStream(string path) {
        MemoryStream result = new MemoryStream();
        byte[] data = File.ReadAllBytes(path);
        for (int i = 0; i < data.Length - 10; i++) {
            if (data[i] == 0x1f && data[i+1] == 0x8b && data[i+2] == 0x08) {
                try {
                    using (MemoryStream ms = new MemoryStream(data, i, data.Length - i)) {
                        using (GZipStream decompressor = new GZipStream(ms, CompressionMode.Decompress)) {
                            decompressor.CopyTo(result);
                        }
                    }
                } catch { }
            }
        }
        result.Position = 0;
        return result;
    }
}
"@
}

# Initial paths
$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN_NAME = if ($IsWindows) { "spring2.exe" } else { "spring2" }
$SPRING_BIN_DEFAULT = Join-Path $BUILD_DIR $SPRING_BIN_NAME
$SPRING_BIN = if ($env:SPRING_BIN) { $env:SPRING_BIN } else { $SPRING_BIN_DEFAULT }
$THREADS = if ($env:THREADS) { $env:THREADS } else { 8 }

$TMP_DIR = Join-Path $SCRIPT_DIR "output"
$TMP_INPUT_DIR = Join-Path $TMP_DIR "input"
$TMP_LOG_DIR = Join-Path $TMP_DIR "logs"
$TMP_OUTPUT_DIR = Join-Path $TMP_DIR "runs"
$TMP_WORK_DIR = Join-Path $TMP_DIR "work"

# Dataset: SRR2990433 (EBI FTP)
$URL_R1 = "ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_1.fastq.gz"
$URL_R2 = "ftp://ftp.sra.ebi.ac.uk/vol1/fastq/SRR818/009/SRR8185389/SRR8185389_2.fastq.gz"

$PATH_R1 = Join-Path $TMP_INPUT_DIR "SRR8185389_1.fastq.gz"
$PATH_R2 = Join-Path $TMP_INPUT_DIR "SRR8185389_2.fastq.gz"

# Helper: Robust FTP/HTTP download with Progress Bar (using native curl for thread safety)
function Invoke-BenchmarkDataDownload {
    param($url, $path)
    if (Test-Path $path) { 
        # If file is smaller than 10MB, it's likely a corrupted/truncated previous attempt
        if ((Get-Item $path).Length -gt 10MB) {
            Write-Host "Using existing file: $(Split-Path $path -Leaf)" -ForegroundColor Gray
            return 
        }
        Write-Host "Existing file $(Split-Path $path -Leaf) is too small, re-downloading..." -ForegroundColor Yellow
        Remove-Item $path -Force
    }
    
    Write-Host "Downloading $(Split-Path $path -Leaf)..." -ForegroundColor Cyan
    Write-Host "From: $url" -ForegroundColor Gray
    
    # Use native curl.exe which is available on modern Windows and handles progress perfectly
    & curl.exe -L -# -o $path $url
    
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Download failed for $url"
        exit 1
    }
}

# Helper: Resource tracking invocation
function Invoke-ResourceLoggedProcess($binary, $arguments) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $binary
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $false
    $psi.RedirectStandardError = $false
    
    $process = [System.Diagnostics.Process]::Start($psi)
    $peakRss = 0
    while (-not $process.HasExited) {
        try {
            $currentRss = $process.PeakWorkingSet64
            if ($currentRss -gt $peakRss) { $peakRss = $currentRss }
        }
        catch {}
        Start-Sleep -Milliseconds 500
    }
    $sw.Stop()
    if ($process.ExitCode -ne 0) {
        throw "Process failed with exit code $($process.ExitCode): $binary $arguments"
    }
    $result = @{}
    $result.elapsed_seconds = $sw.Elapsed.TotalSeconds
    $result.user_seconds = $process.UserProcessorTime.TotalSeconds
    $result.system_seconds = $process.PrivilegedProcessorTime.TotalSeconds
    $result.max_rss_kb = [Math]::Round($peakRss / 1024)
    $result.cpu_percent = "{0:P0}" -f (($result.user_seconds + $result.system_seconds) / $result.elapsed_seconds)
    return $result
}

# Ensure environment
function Initialize-BenchmarkEnv {
    New-Item -ItemType Directory -Path $TMP_INPUT_DIR, $TMP_LOG_DIR, $TMP_OUTPUT_DIR, $TMP_WORK_DIR -Force | Out-Null
    
    Invoke-BenchmarkDataDownload $URL_R1 $PATH_R1
    Invoke-BenchmarkDataDownload $URL_R2 $PATH_R2

    # SETUP PATHS
    $global:INPUT_STEM = "SRR8185389_pe"
    $global:OUTPUT_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
    $global:DECOMP_BASE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")
    
    $global:DECOMP_FILE_1 = "$global:DECOMP_BASE.1"
    $global:DECOMP_FILE_2 = "$global:DECOMP_BASE.2"

    # Setup unique work directory
    $global:WORK_DIR = Join-Path $TMP_WORK_DIR "$INPUT_STEM.work"
    if (Test-Path $global:WORK_DIR) { Remove-Item $global:WORK_DIR -Recurse -Force }
    New-Item -ItemType Directory -Path $global:WORK_DIR -Force | Out-Null
}

function Initialize-SpringBinary {
    if (Test-Path $SPRING_BIN) { return }
    Write-Host "Spring binary not found; building..." -ForegroundColor Yellow
    $buildLog = Join-Path $TMP_LOG_DIR "build.log"
    $env:CC = "gcc"; $env:CXX = "g++"
    $cmakeConfigArgs = @("-S", "$ROOT_DIR", "-B", "$BUILD_DIR", "-G", "Ninja", "-DSPRING_STATIC_RUNTIMES=OFF")
    & cmake @cmakeConfigArgs 2>&1 | Set-Content $buildLog
    & cmake --build "$BUILD_DIR" --parallel 2>&1 | Add-Content $buildLog
    if ($IsWindows) { & cmake --build "$BUILD_DIR" --target copy_runtime_dlls 2>&1 | Add-Content $buildLog }
}

# --- Main Logic ---
Initialize-BenchmarkEnv
Initialize-SpringBinary

# Cleanup previous
foreach ($f in @($global:OUTPUT_FILE, $global:DECOMP_FILE_1, $global:DECOMP_FILE_2)) {
    if (Test-Path $f) { Remove-Item $f -Force }
}

# --- Compression ---
Write-Host "Running Spring paired-end compression (SRR2990433)" -ForegroundColor Cyan
Write-Host "  R1:      $PATH_R1"
Write-Host "  R2:      $PATH_R2"
Write-Host "  threads: $THREADS"

$compArgs = "-c -i `"$PATH_R1`" `"$PATH_R2`" -o `"$global:OUTPUT_FILE`" -w `"$global:WORK_DIR`" -t $THREADS -q lossless -n `"Big Benchmark SRR2990433`""
$compResults = Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs

# --- Decompression ---
Write-Host "`nRunning Spring decompression" -ForegroundColor Cyan
$decompArgs = "-d -i `"$global:OUTPUT_FILE`" -o `"$global:DECOMP_BASE`" -w `"$global:WORK_DIR`""
$decompResults = Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs

# --- Results ---
$inputSize = (Get-Item $PATH_R1).Length + (Get-Item $PATH_R2).Length
$outputSize = (Get-Item $global:OUTPUT_FILE).Length
$decompSize = (Get-Item $global:DECOMP_FILE_1).Length + (Get-Item $global:DECOMP_FILE_2).Length

$reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }
$ratio = if ($outputSize -gt 0) { $inputSize / $outputSize } else { 0 }

# Integrity Check
Write-Host "`nVerifying integrity..." -ForegroundColor Gray
function Get-DecompHash($path) {
    if ($path.EndsWith(".gz")) {
        $ds = [BigBencher]::GetDecompressedStream($path)
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $hashBytes = $hasher.ComputeHash($ds)
        $ds.Dispose()
    }
    else {
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        $fs = [System.IO.File]::OpenRead($path)
        $hashBytes = $hasher.ComputeHash($fs)
        $fs.Dispose()
    }
    return [System.BitConverter]::ToString($hashBytes).Replace("-", "").ToLower()
}

$hash_orig_1 = Get-DecompHash $PATH_R1
$hash_orig_2 = Get-DecompHash $PATH_R2
$hash_decomp_1 = Get-DecompHash $global:DECOMP_FILE_1
$hash_decomp_2 = Get-DecompHash $global:DECOMP_FILE_2

$status1 = if ($hash_orig_1 -eq $hash_decomp_1) { "match" } else { "mismatch" }
$status2 = if ($hash_orig_2 -eq $hash_decomp_2) { "match" } else { "mismatch" }

# Reporting
Write-Output "`nBenchmark result (Paired-End combined)"
Write-Output ("  original bytes:   {0:N0}" -f $inputSize)
Write-Output ("  compressed bytes: {0:N0}" -f $outputSize)
Write-Output ("  decompressed bytes: {0:N0}" -f $decompSize)
Write-Output ("  size reduction:   {0:N2}%" -f $reduction)
Write-Output ("  compression ratio {0:N3}x" -f $ratio)

Write-Output "`nCompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $compResults.elapsed_seconds)
Write-Output ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $compResults.max_rss_kb, ($compResults.max_rss_kb / 1024))

Write-Output "`nDecompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $decompResults.elapsed_seconds)
Write-Output ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $decompResults.max_rss_kb, ($decompResults.max_rss_kb / 1024))

Write-Output "`nRound-trip check"
Write-Output "  Read 1 status: $status1"
Write-Output "  Read 2 status: $status2"
Write-Output "  Overall status: $(if ($status1 -eq 'match' -and $status2 -eq 'match') { 'PASSED' } else { 'FAILED' })"
