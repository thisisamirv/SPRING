# benchmark/run_big_benchmark.ps1 - Stress test and performance metrics for SPRING2 on Windows
$ErrorActionPreference = 'Stop'

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
    
    # Use native .NET GZipStream for high performance without gzip.exe dependency
    $fileStream = [System.IO.File]::OpenRead($path)
    $stream = if ($path.EndsWith(".gz")) {
        $gzipStream = New-Object System.IO.Compression.GZipStream($fileStream, [System.IO.Compression.CompressionMode]::Decompress)
        New-Object System.IO.StreamReader($gzipStream)
    } else {
        New-Object System.IO.StreamReader($fileStream)
    }

    try {
        $buffer = New-Object byte[] 65536
        while (($line = $stream.ReadLine()) -ne $null) {
            $lineCount++
            # FASTQ read line is always (4n + 2)
            if ($lineCount % 4 -eq 2) {
                if ($line.Length -gt $maxLen) { $maxLen = $line.Length }
            }
        }
    } finally {
        $stream.Dispose()
        if ($gzipStream) { $gzipStream.Dispose() }
        $fileStream.Dispose()
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
        } catch {}
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
        return
    }

    if (-not (Test-Path $DEFAULT_INPUT_FASTQ)) {
        Write-Host "Benchmark input not found: $DEFAULT_INPUT_FASTQ" -ForegroundColor Red
        Write-Host "Please download it from: $DOWNLOAD_URL"
        Write-Host "Or specify INPUT_FASTQ environment variable."
        exit 1
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

$INPUT_ABS = (Get-Item $INPUT_FASTQ).FullName
$INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($INPUT_ABS))
$WORK_DIR = (Join-Path $TMP_WORK_DIR "$INPUT_STEM.work").Replace("/", "\")
$OUTPUT_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
$DECOMP_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")

Write-Host "Analyzing FASTQ..." -ForegroundColor Gray
$maxReadLen = Get-MaxReadLength $INPUT_ABS

# Cleanup previous
if (Test-Path $WORK_DIR) { Remove-Item $WORK_DIR -Recurse -Force }
New-Item -ItemType Directory -Path $WORK_DIR -Force | Out-Null
if (Test-Path $OUTPUT_FILE) { Remove-Item $OUTPUT_FILE -Force }
if (Test-Path $DECOMP_FILE) { Remove-Item $DECOMP_FILE -Force }

# --- Compression ---
Write-Host "Running Spring lossless compression" -ForegroundColor Cyan
Write-Host "  input:   $INPUT_ABS"
Write-Host "  output:  $OUTPUT_FILE"
Write-Host "  threads: $THREADS"

$compArgs = "-c -i `"$INPUT_ABS`" -o `"$OUTPUT_FILE`" -w `"$WORK_DIR`" -t $THREADS -q lossless"
$compResults = Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs

# --- Decompression ---
Write-Host "`nRunning Spring decompression" -ForegroundColor Cyan
Write-Host "  input:   $OUTPUT_FILE"
Write-Host "  output:  $DECOMP_FILE"

$decompArgs = "-d -i `"$OUTPUT_FILE`" -o `"$DECOMP_FILE`""
$decompResults = Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs

# --- Results ---
$inputSize = (Get-Item $INPUT_ABS).Length
$outputSize = (Get-Item $OUTPUT_FILE).Length
$decompSize = (Get-Item $DECOMP_FILE).Length

$reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }
$ratio = if ($outputSize -gt 0) { $inputSize / $outputSize } else { 0 }

# Integrity Check
Write-Host "`nVerifying integrity..." -ForegroundColor Gray
$originalHash = if ($INPUT_ABS.EndsWith(".gz")) {
    Write-Host "  Hashing original (decompressed) content..." -ForegroundColor Gray
    $fs = [System.IO.File]::OpenRead($INPUT_ABS)
    $gs = New-Object System.IO.Compression.GZipStream($fs, [System.IO.Compression.CompressionMode]::Decompress)
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    $hashBytes = $hasher.ComputeHash($gs)
    $gs.Dispose(); $fs.Dispose()
    [System.BitConverter]::ToString($hashBytes).Replace("-", "").ToLower()
} else {
    (Get-FileHash -Path $INPUT_ABS -Algorithm SHA256).Hash.ToLower()
}

$decompHash = (Get-FileHash -Path $DECOMP_FILE -Algorithm SHA256).Hash.ToLower()
$checksumStatus = if ($originalHash -eq $decompHash) { "match" } else { "mismatch" }

# Reporting
Write-Output "`nBenchmark result"
Write-Output ("  original bytes:   {0:N0}" -f $inputSize)
Write-Output ("  compressed bytes: {0:N0}" -f $outputSize)
Write-Output ("  decompressed bytes: {0:N0}" -f $decompSize)
Write-Output ("  size reduction:   {0:N2}%" -f $reduction)
Write-Output ("  compression ratio {0:N3}x" -f $ratio)

Write-Output "`nCompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $compResults.elapsed_seconds)
Write-Output ("  cpu usage:        {0}" -f $compResults.cpu_percent)
Write-Output ("  cpu time:         user {0:N3}s, system {1:N3}s" -f $compResults.user_seconds, $compResults.system_seconds)
Write-Output ("  avg core usage:   {0:N2} cores" -f (($compResults.user_seconds + $compResults.system_seconds) / $compResults.elapsed_seconds))
Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $compResults.max_rss_kb, ($compResults.max_rss_kb/1024))

Write-Output "`nDecompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $decompResults.elapsed_seconds)
Write-Output ("  cpu usage:        {0}" -f $decompResults.cpu_percent)
Write-Output ("  cpu time:         user {0:N3}s, system {1:N3}s" -f $decompResults.user_seconds, $decompResults.system_seconds)
Write-Output ("  avg core usage:   {0:N2} cores" -f (($decompResults.user_seconds + $decompResults.system_seconds) / $decompResults.elapsed_seconds))
Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $decompResults.max_rss_kb, ($decompResults.max_rss_kb/1024))

Write-Output "`nRound-trip check"
Write-Output "  original hash:    $originalHash"
Write-Output "  decompressed hash:$decompHash"
Write-Output "  checksum status:  $checksumStatus"
Write-Output "  decompressed file matches input: $(if ($checksumStatus -eq 'match') { 'identical' } else { 'different' })"
