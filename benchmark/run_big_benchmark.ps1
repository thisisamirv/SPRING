$ErrorActionPreference = 'Stop'

# Helper: GZip decompression for multi-member files (.gz/BGZF)
# Built-in .NET GZipStream in PowerShell 5.1 stops after the first member.
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.IO.Compression;

public class GZipHelper {
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

# Initial paths
$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN_NAME = if ($IsWindows) { "spring2.exe" } else { "spring2" }
$SPRING_BIN_DEFAULT = Join-Path $BUILD_DIR $SPRING_BIN_NAME
$SPRING_BIN = if ($env:SPRING_BIN) { $env:SPRING_BIN } else { $SPRING_BIN_DEFAULT }
$THREADS = if ($env:THREADS) { $env:THREADS } else { 8 }

$TMP_DIR = Join-Path $SCRIPT_DIR "tmp"
$TMP_INPUT_DIR = Join-Path $TMP_DIR "input"
$TMP_LOG_DIR = Join-Path $TMP_DIR "logs"
$TMP_OUTPUT_DIR = Join-Path $TMP_DIR "output"
$TMP_WORK_DIR = Join-Path $TMP_DIR "work"

$DOWNLOAD_URL = "https://figshare.com/ndownloader/files/38965664"
$DEFAULT_INPUT_FASTQ = Join-Path $TMP_INPUT_DIR "04-CC002-659-M_S4_L001_R2_001.fastq.gz"
$INPUT_FASTQ = if ($env:INPUT_FASTQ) { $env:INPUT_FASTQ } else { $DEFAULT_INPUT_FASTQ }

# Helper: Fast FASTQ read length detection
function Get-MaxReadLength($path) {
    if (-not (Test-Path $path)) { return 0 }
    
    $maxLen = 0
    $lineCount = 0
    
    # Use robust GZip streaming for multi-member files
    $stream = if ($path.EndsWith(".gz")) {
        $decompStream = [GZipHelper]::GetDecompressedStream($path)
        New-Object System.IO.StreamReader($decompStream)
    }
    else {
        New-Object System.IO.StreamReader([System.IO.File]::OpenRead($path))
    }

    try {
        while ($null -ne ($line = $stream.ReadLine())) {
            $lineCount++
            # FASTQ read line is always (4n + 2)
            if ($lineCount % 4 -eq 2) {
                if ($line.Length -gt $maxLen) { $maxLen = $line.Length }
            }
        }
    }
    finally {
        $stream.Dispose()
    }
    
    return $maxLen
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
        Start-Sleep -Milliseconds 100
    }
    
    $sw.Stop()
    
    if ($process.ExitCode -ne 0) {
        $msg = "Process failed with exit code $($process.ExitCode): $binary $arguments"
        Write-Host "`nERROR: $msg" -ForegroundColor Red
        throw $msg
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
    
    if ($INPUT_FASTQ -ne $DEFAULT_INPUT_FASTQ) {
        if (-not (Test-Path $INPUT_FASTQ)) {
            Write-Error "Requested INPUT_FASTQ does not exist: $INPUT_FASTQ"
            exit 1
        }
    }
    elseif (-not (Test-Path $DEFAULT_INPUT_FASTQ)) {
        Write-Host "Benchmark input not found: $DEFAULT_INPUT_FASTQ" -ForegroundColor Red
        Write-Host "Please download it from: $DOWNLOAD_URL"
        Write-Host "Or specify INPUT_FASTQ environment variable."
        exit 1
    }

    # SETUP PATHS
    $global:INPUT_ABS = (Get-Item $INPUT_FASTQ).FullName
    $global:INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($INPUT_ABS))
    $global:OUTPUT_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
    $global:DECOMP_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")

    # Decompress to temporary file for analysis and integrity check
    # This avoids .NET GZipStream limitations in PowerShell 5.1
    $global:INPUT_RAW = Join-Path $TMP_INPUT_DIR "input_raw.fastq"
    if ($INPUT_FASTQ.EndsWith(".gz")) {
        if (-not (Test-Path $global:INPUT_RAW)) {
            Write-Host "Decompressing input for analysis..." -ForegroundColor Gray
            try {
                [GZipHelper]::Decompress($INPUT_FASTQ, $global:INPUT_RAW)
            } catch {
                Write-Error "Failed to decompress input file: $_"
                exit 1
            }
        }
    }
    else {
        $global:INPUT_RAW = $INPUT_FASTQ
    }

    # Setup unique work directory to avoid "file in use" errors from previous runs
    $workDirBase = Join-Path $TMP_WORK_DIR "$INPUT_STEM.work"
    $global:WORK_DIR = $workDirBase
    $retry = 0
    while (Test-Path $global:WORK_DIR) {
        try {
            Remove-Item $global:WORK_DIR -Recurse -Force -ErrorAction Stop
            break
        } catch {
            $retry++
            $global:WORK_DIR = "$workDirBase.$retry"
            if ($retry -gt 10) { break }
        }
    }
}

function Ensure-SpringBinary {
    if (Test-Path $SPRING_BIN) { return }
    
    Write-Host "Spring binary not found; building..." -ForegroundColor Yellow
    $buildLog = Join-Path $TMP_LOG_DIR "build.log"

    # Set environment variables for MinGW as specified by user
    $env:CC = "gcc"
    $env:CXX = "g++"

    $cmakeConfigArgs = @(
        "-S", "$ROOT_DIR", 
        "-B", "$BUILD_DIR", 
        "-G", "Ninja", 
        "-DSPRING_STATIC_RUNTIMES=OFF", 
        "-Dspring_optimize_for_native=OFF", 
        "-Dspring_optimize_for_portability=ON"
    )

    Write-Host "Running CMake configure (Ninja)..." -ForegroundColor Gray
    & cmake @cmakeConfigArgs 2>&1 | Set-Content $buildLog
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed. See $buildLog" }
    
    Write-Host "Running CMake build..." -ForegroundColor Gray
    & cmake --build "$BUILD_DIR" --parallel 2>&1 | Add-Content $buildLog
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed. See $buildLog" }

    Write-Host "Copying runtime DLLs..." -ForegroundColor Gray
    & cmake --build "$BUILD_DIR" --target copy_runtime_dlls 2>&1 | Add-Content $buildLog
}

# --- Main Logic ---

Initialize-BenchmarkEnv
Ensure-SpringBinary

Write-Host "Analyzing FASTQ..." -ForegroundColor Gray
$maxReadLen = Get-MaxReadLength $global:INPUT_RAW

# Cleanup previous (WORK_DIR already handled in Initialize-BenchmarkEnv)
if (Test-Path $global:OUTPUT_FILE) { Remove-Item $global:OUTPUT_FILE -Force }
if (Test-Path $global:DECOMP_FILE) { Remove-Item $global:DECOMP_FILE -Force }

# --- Compression ---
Write-Host "Running Spring lossless compression" -ForegroundColor Cyan
Write-Host "  input:   $global:INPUT_ABS"
Write-Host "  output:  $global:OUTPUT_FILE"
Write-Host "  threads: $THREADS"

$compArgs = "-c -i `"$global:INPUT_ABS`" -o `"$global:OUTPUT_FILE`" -w `"$global:WORK_DIR`" -t $THREADS -q lossless -n `"Big Benchmark`""
$compResults = Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs

# --- Decompression ---
Write-Host "`nRunning Spring decompression" -ForegroundColor Cyan
Write-Host "  input:   $global:OUTPUT_FILE"
Write-Host "  output:  $global:DECOMP_FILE"

$decompArgs = "-d -i `"$global:OUTPUT_FILE`" -o `"$global:DECOMP_FILE`""
$decompResults = Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs

# --- Results ---
$inputSize = (Get-Item $global:INPUT_ABS).Length
$outputSize = (Get-Item $global:OUTPUT_FILE).Length
$decompSize = (Get-Item $global:DECOMP_FILE).Length

$reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }
$ratio = if ($outputSize -gt 0) { $inputSize / $outputSize } else { 0 }

# Integrity Check (Normalized for GZIP)
Write-Host "`nVerifying integrity..." -ForegroundColor Gray
$integrity_method = "SHA-256 binary hash"
$originalHash = if ($INPUT_ABS.EndsWith(".gz")) {
    Write-Host "  Hashing original (decompressed) input content..." -ForegroundColor Gray
    $ds = [GZipHelper]::GetDecompressedStream($INPUT_ABS)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($ds)
    $ds.Dispose()
    [System.BitConverter]::ToString($hashBytes).Replace("-", "").ToLower()
}
else {
    (Get-FileHash -Path $INPUT_ABS -Algorithm SHA256).Hash.ToLower()
}

$decompHash = (Get-FileHash -Path $DECOMP_FILE -Algorithm SHA256).Hash.ToLower()
$checksumStatus = if ($originalHash -eq $decompHash) { "match" } else { "mismatch" }

# Reporting (Aligned with run_big_benchmark.sh)
Write-Output "`nBenchmark result"
Write-Output ("  original bytes:   {0}" -f $inputSize)
Write-Output ("  compressed bytes: {0}" -f $outputSize)
Write-Output ("  decompressed bytes: {0}" -f $decompSize)
Write-Output ("  size reduction:   {0:N2}%" -f $reduction)
Write-Output ("  compression ratio {0:N3}x" -f $ratio)

Write-Output "`nCompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $compResults.elapsed_seconds)
Write-Output ("  cpu usage:        {0}" -f $compResults.cpu_percent)
Write-Output ("  cpu time:         user {0:N3}s, system {1:N3}s" -f $compResults.user_seconds, $compResults.system_seconds)
Write-Output ("  avg core usage:   {0:N2} cores" -f (($compResults.user_seconds + $compResults.system_seconds) / $compResults.elapsed_seconds))
Write-Output ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $compResults.max_rss_kb, ($compResults.max_rss_kb / 1024))

Write-Output "`nDecompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $decompResults.elapsed_seconds)
Write-Output ("  cpu usage:        {0}" -f $decompResults.cpu_percent)
Write-Output ("  cpu time:         user {0:N3}s, system {1:N3}s" -f $decompResults.user_seconds, $decompResults.system_seconds)
Write-Output ("  avg core usage:   {0:N2} cores" -f (($decompResults.user_seconds + $decompResults.system_seconds) / $decompResults.elapsed_seconds))
Write-Output ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $decompResults.max_rss_kb, ($decompResults.max_rss_kb / 1024))

Write-Output "`nRound-trip check"
Write-Output ("  integrity method: {0}" -f $integrity_method)
Write-Output ("  original checksum: {0}" -f $originalHash)
Write-Output ("  output checksum:   {0}" -f $decompHash)
Write-Output ("  checksum status:  {0}" -f $checksumStatus)
Write-Output ("  decompressed file matches input: {0}" -f $(if ($checksumStatus -eq 'match') { "identical" } else { "different" }))
