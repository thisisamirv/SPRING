# benchmark/run_lossless_benchmark.ps1 - Lossless performance metrics for SPRING2 on Windows
$ErrorActionPreference = 'Stop'

# Initial paths
$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN_NAME = if ($IsWindows) { "spring2.exe" } else { "spring2" }
$SPRING_BIN_DEFAULT = Join-Path $BUILD_DIR $SPRING_BIN_NAME
$SPRING_BIN = if ($env:SPRING_BIN) { $env:SPRING_BIN } else { $SPRING_BIN_DEFAULT }
$THREADS = if ($env:THREADS) { [int]$env:THREADS } else { 8 }

$INPUT_DIR = Join-Path $ROOT_DIR "assets\sample-data"
$OUTPUT_BASE = Join-Path $SCRIPT_DIR "output"
$LOG_DIR = Join-Path $OUTPUT_BASE "logs"
$OUTPUT_DIR = Join-Path $OUTPUT_BASE "runs"
$WORK_ROOT_DIR = Join-Path $OUTPUT_BASE "work"

$DEFAULT_INPUT_FASTQ = Join-Path $INPUT_DIR "sample.fastq"
$INPUT_FILE_ARG = if ($args.Count -gt 0) { $args[0] } else { $DEFAULT_INPUT_FASTQ }

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
    }
    else {
        New-Object System.IO.StreamReader($fileStream)
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
        $stream.Close()
        $fileStream.Close()
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
function Initialize-Environment {
    New-Item -ItemType Directory -Path $INPUT_DIR, $LOG_DIR, $OUTPUT_DIR, $WORK_ROOT_DIR -Force | Out-Null
    
    if (-not (Test-Path $INPUT_FILE_ARG)) {
        Write-Error "Input FASTQ not found: $INPUT_FILE_ARG"
        exit 1
    }
}

function Initialize-SpringBinary {
    if (Test-Path $SPRING_BIN) { return }
    
    Write-Host "Spring binary not found; building..." -ForegroundColor Yellow
    $buildLog = Join-Path $LOG_DIR "build.log"

    # Set environment variables for MinGW if applicable
    if ($IsWindows) {
        $env:CC = "gcc"
        $env:CXX = "g++"
    }

    $cmakeConfigArgs = @(
        "-S", "$ROOT_DIR", 
        "-B", "$BUILD_DIR", 
        "-DSPRING_STATIC_RUNTIMES=OFF", 
        "-Dspring_optimize_for_native=OFF", 
        "-Dspring_optimize_for_portability=ON"
    )

    # Use Ninja if available, otherwise default
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($null -ne $ninja) {
        $cmakeConfigArgs += "-G", "Ninja"
    }

    Write-Host "Running CMake configure..." -ForegroundColor Gray
    & cmake @cmakeConfigArgs 2>&1 | Set-Content $buildLog
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed. See $buildLog" }
    
    Write-Host "Running CMake build..." -ForegroundColor Gray
    & cmake --build "$BUILD_DIR" --parallel 2>&1 | Add-Content $buildLog
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed. See $buildLog" }

    if ($IsWindows) {
        Write-Host "Copying runtime DLLs..." -ForegroundColor Gray
        & cmake --build "$BUILD_DIR" --target copy_runtime_dlls 2>&1 | Add-Content $buildLog
    }
}

# --- Main Logic ---

Initialize-Environment
Initialize-SpringBinary

$INPUT_ABS = (Get-Item $INPUT_FILE_ARG).FullName
$INPUT_BASENAME = [System.IO.Path]::GetFileName($INPUT_ABS)
# Stem is filename without extension (handle .fastq.gz)
$INPUT_STEM = $INPUT_BASENAME
if ($INPUT_STEM.EndsWith(".gz")) { $INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension($INPUT_STEM) }
$INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension($INPUT_STEM)

$WORK_DIR = (Join-Path $WORK_ROOT_DIR "$INPUT_STEM.work").Replace("/", "\")
$OUTPUT_FILE = (Join-Path $OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
$DECOMP_FILE = (Join-Path $OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")

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
Write-Host "  workdir: $WORK_DIR"
Write-Host "  threads: $THREADS"
Write-Host "  max read length: $maxReadLen"
Write-Host "  mode:    lossless"

$compArgs = "-c -i `"$INPUT_ABS`" -o `"$OUTPUT_FILE`" -w `"$WORK_DIR`" -t $THREADS -q lossless"
$compResults = Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs

# --- Decompression ---
Write-Host "`nRunning Spring decompression" -ForegroundColor Cyan
Write-Host "  input:   $OUTPUT_FILE"
Write-Host "  output:  $DECOMP_FILE"

$decompArgs = "-d -i `"$OUTPUT_FILE`" -o `"$DECOMP_FILE`" -w `"$WORK_DIR`""
$decompResults = Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs

# Cleanup work dir
if (Test-Path $WORK_DIR) {
    if ((Get-ChildItem $WORK_DIR).Count -eq 0) {
        Remove-Item $WORK_DIR -Recurse -Force
    }
}

# --- Results ---
$inputSize = (Get-Item $INPUT_ABS).Length
$outputSize = (Get-Item $OUTPUT_FILE).Length
$decompSize = (Get-Item $DECOMP_FILE).Length

$reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }
$ratio = if ($outputSize -gt 0) { $inputSize / $outputSize } else { 0 }

# Integrity Check
Write-Host "`nVerifying integrity..." -ForegroundColor Gray
$originalHash = Get-FileHash -Path $INPUT_ABS -Algorithm SHA256
$decompHash = Get-FileHash -Path $DECOMP_FILE -Algorithm SHA256
$checksumStatus = if ($originalHash.Hash -eq $decompHash.Hash) { "match" } else { "mismatch" }

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
Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $compResults.max_rss_kb, ($compResults.max_rss_kb / 1024))

Write-Output "`nDecompression resources"
Write-Output ("  elapsed time:     {0:N3}s" -f $decompResults.elapsed_seconds)
Write-Output ("  cpu usage:        {0}" -f $decompResults.cpu_percent)
Write-Output ("  cpu time:         user {0:N3}s, system {1:N3}s" -f $decompResults.user_seconds, $decompResults.system_seconds)
Write-Output ("  avg core usage:   {0:N2} cores" -f (($decompResults.user_seconds + $decompResults.system_seconds) / $decompResults.elapsed_seconds))
Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $decompResults.max_rss_kb, ($decompResults.max_rss_kb / 1024))

Write-Output "`nRound-trip check"
Write-Output "  original hash:    $($originalHash.Hash)"
Write-Output "  decompressed hash:$($decompHash.Hash)"
Write-Output "  checksum status:  $checksumStatus"
Write-Output "  decompressed file matches input: $(if ($checksumStatus -eq 'match') { 'identical' } else { 'different' })"
