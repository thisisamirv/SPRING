# benchmark/run_comparison_benchmark.ps1 - Comparative performance metrics for SPRING2 on Windows
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
$SPRING_V1_ENV_NAME = "spring_v1"

# --- Helpers ---

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
        while (($line = $stream.ReadLine()) -ne $null) {
            $lineCount++
            if ($lineCount % 4 -eq 2) {
                if ($line.Length -gt $maxLen) { $maxLen = $line.Length }
            }
        }
    } finally {
        $stream.Close()
    }
    return $maxLen
}

function Invoke-ResourceLoggedProcess($binary, $arguments) {
    # Check if binary is a full path or just a command
    $fullBinary = if (Test-Path $binary) { (Get-Item $binary).FullName } else { $binary }
    
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $fullBinary
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    
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
        throw "Process failed with exit code $($process.ExitCode): $binary $arguments"
    }

    return [PSCustomObject]@{
        elapsed_seconds = $sw.Elapsed.TotalSeconds
        user_seconds = $process.UserProcessorTime.TotalSeconds
        system_seconds = $process.PrivilegedProcessorTime.TotalSeconds
        max_rss_kb = [Math]::Round($peakRss / 1024)
        cpu_percent = "{0:P1}" -f (($process.TotalProcessorTime.TotalSeconds) / $sw.Elapsed.TotalSeconds)
    }
}

function Ensure-SpringBinary {
    if (Test-Path $SPRING_BIN) { return }
    
    Write-Host "Spring binary not found; building with proven configuration..." -ForegroundColor Yellow
    $buildLog = Join-Path $TMP_LOG_DIR "build.log"
    
    # Set environment variables for MinGW
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
    if ($LASTEXITCODE -ne 0) { throw "Build failed. Check $buildLog" }

    Write-Host "Copying runtime DLLs..." -ForegroundColor Gray
    & cmake --build "$BUILD_DIR" --target copy_runtime_dlls 2>&1 | Add-Content $buildLog
}

function Get-SpringV1Runner {
    $mamba = Get-Command mamba -ErrorAction SilentlyContinue
    if (-not $mamba) { $mamba = Get-Command conda -ErrorAction SilentlyContinue }
    
    if (-not $mamba) {
        Write-Error "Mamba/Conda not found. Install it and create environment '$SPRING_V1_ENV_NAME' with legacy spring."
        exit 1
    }

    # Verify environment
    & $mamba.Source env list | Select-String $SPRING_V1_ENV_NAME | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Mamba environment '$SPRING_V1_ENV_NAME' not found."
        exit 1
    }

    # Verify spring exists in env
    & $mamba.Source run -n $SPRING_V1_ENV_NAME spring --version 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Error "'spring' (v1) not found in environment '$SPRING_V1_ENV_NAME'."
        exit 1
    }

    return $mamba.Source
}

function Run-BenchmarkTask($label, $displayName, $runnerBinary, $isV1, $inputPath, $maxReadLen) {
    Write-Host "`n--- Running $displayName ---" -ForegroundColor Cyan
    
    $inputAbs = (Get-Item $inputPath).FullName
    $inputStem = [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($inputAbs))
    $envName = if ($isV1) { "spring_v1" } else { "current" }
    $workDir = (Join-Path $TMP_WORK_DIR ("$inputStem" + "_" + $envName)).Replace("/", "\")
    $outputFile = (Join-Path $TMP_OUTPUT_DIR ("$inputStem" + "_" + $envName + ".sp")).Replace("/", "\")
    $decompFile = Join-Path $TMP_OUTPUT_DIR "$inputStem.$label.roundtrip.fastq"

    if (Test-Path $workDir) { Remove-Item $workDir -Recurse -Force }
    New-Item -ItemType Directory -Path $workDir -Force | Out-Null
    
    # Arguments
    $compArgsList = @("-c", "-i", "`"$inputPath`"", "-o", "`"$outputFile`"", "-w", "`"$workDir`"", "-t", $THREADS, "-q", "lossless")
    
    # v1 specific: long-read mode
    if ($isV1 -and $maxReadLen -gt 511) {
        $compArgsList += "-l"
    }

    # Wrap in mamba run if needed
    $actualBinary = $runnerBinary
    $actualArgs = $compArgsList -join " "
    if ($isV1) {
        $actualBinary = $runnerBinary # This is 'mamba'
        $actualArgs = "run -n $SPRING_V1_ENV_NAME spring " + $actualArgs
    }

    Write-Host "Compressing..." -ForegroundColor Gray
    $compMetrics = Invoke-ResourceLoggedProcess $actualBinary $actualArgs

    Write-Host "Decompressing..." -ForegroundColor Gray
    $decompArgsList = @("-d", "-i", "`"$outputFile`"", "-o", "`"$decompFile`"")
    $actualDecompArgs = if ($isV1) { "run -n $SPRING_V1_ENV_NAME spring " + ($decompArgsList -join " ") } else { $decompArgsList -join " " }
    $decompMetrics = Invoke-ResourceLoggedProcess $actualBinary $actualDecompArgs

    # Verify integrity
    $originalHash = (Get-FileHash $inputPath -Algorithm SHA256).Hash
    $decompressedHash = (Get-FileHash $decompFile -Algorithm SHA256).Hash
    $match = $originalHash -eq $decompressedHash

    return [PSCustomObject]@{
        Label = $label
        DisplayName = $displayName
        InputSize = (Get-Item $inputPath).Length
        OutputSize = (Get-Item $outputFile).Length
        DecompSize = (Get-Item $decompFile).Length
        CompMetrics = $compMetrics
        DecompMetrics = $decompMetrics
        HashMatch = $match
    }
}

function Print-Report($metrics) {
    $ratio = if ($metrics.OutputSize -gt 0) { $metrics.InputSize / $metrics.OutputSize } else { 0 }
    $reduction = if ($metrics.InputSize -gt 0) { ($metrics.InputSize - $metrics.OutputSize) * 100 / $metrics.InputSize } else { 0 }

    Write-Output "`nBenchmark result ($($metrics.DisplayName))"
    Write-Output ("  original bytes:   {0:N0}" -f $metrics.InputSize)
    Write-Output ("  compressed bytes: {0:N0}" -f $metrics.OutputSize)
    Write-Output ("  size reduction:   {0:N2}%" -f $reduction)
    Write-Output ("  compression ratio {0:N3}x" -f $ratio)

    Write-Output "`nCompression resources ($($metrics.DisplayName))"
    Write-Output ("  elapsed time:     {0:N3}s" -f $metrics.CompMetrics.elapsed_seconds)
    Write-Output ("  cpu usage:        {0}" -f $metrics.CompMetrics.cpu_percent)
    Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $metrics.CompMetrics.max_rss_kb, ($metrics.CompMetrics.max_rss_kb/1024))

    Write-Output "`nDecompression resources ($($metrics.DisplayName))"
    Write-Output ("  elapsed time:     {0:N3}s" -f $metrics.DecompMetrics.elapsed_seconds)
    Write-Output ("  peak memory:      {0:N0} KB ({1:N2} MB RSS)" -f $metrics.DecompMetrics.max_rss_kb, ($metrics.DecompMetrics.max_rss_kb/1024))

    Write-Output "`nRound-trip status ($($metrics.DisplayName)): $(if ($metrics.HashMatch) { 'IDENTICAL' } else { 'DIFFERENT' })"
}

function Print-Comparison($current, $v1) {
    Write-Output "`n========================================"
    Write-Output "       COMPARISON SUMMARY"
    Write-Output "========================================"
    
    $sizeDelta = $v1.OutputSize - $current.OutputSize
    Write-Output ("Current Spring Output:  {0:N0} bytes" -f $current.OutputSize)
    Write-Output ("Spring v1 Output:       {0:N0} bytes" -f $v1.OutputSize)
    
    if ($sizeDelta -gt 0) {
        Write-Output ("WINNER (Size):          Current Spring by {0:N0} bytes ({1:N2}% better)" -f $sizeDelta, ($sizeDelta * 100 / $v1.OutputSize))
    } elseif ($sizeDelta -lt 0) {
        Write-Output ("WINNER (Size):          Spring v1 by {0:N0} bytes ({1:N2}% better)" -f (-$sizeDelta), (-$sizeDelta * 100 / $current.OutputSize))
    } else {
        Write-Output "WINNER (Size):          TIE"
    }

    Write-Output ("`nCurrent Comp Time:      {0:N3}s" -f $current.CompMetrics.elapsed_seconds)
    Write-Output ("Spring v1 Comp Time:   {0:N3}s" -f $v1.CompMetrics.elapsed_seconds)
    
    $timeDelta = $v1.CompMetrics.elapsed_seconds - $current.CompMetrics.elapsed_seconds
    if ($timeDelta -gt 0) {
        Write-Output ("WINNER (Speed):         Current Spring is {0:N2}x faster" -f ($v1.CompMetrics.elapsed_seconds / $current.CompMetrics.elapsed_seconds))
    } elseif ($timeDelta -lt 0) {
        Write-Output ("WINNER (Speed):         Spring v1 is {0:N2}x faster" -f ($current.CompMetrics.elapsed_seconds / $v1.CompMetrics.elapsed_seconds))
    } else {
        Write-Output "WINNER (Speed):         TIE"
    }
    Write-Host ""
}

# --- Main ---

New-Item -ItemType Directory -Path $TMP_INPUT_DIR, $TMP_LOG_DIR, $TMP_OUTPUT_DIR, $TMP_WORK_DIR -Force | Out-Null

if (-not (Test-Path $INPUT_FASTQ)) {
    Write-Error "Benchmark input not found: $INPUT_FASTQ"
    exit 1
}

Ensure-SpringBinary
$mambaBin = Get-SpringV1Runner

$INPUT_ABS = (Get-Item $INPUT_FASTQ).FullName
Write-Host "Analyzing FASTQ..." -ForegroundColor Gray
$maxReadLen = Get-MaxReadLength $INPUT_ABS

$currentMetrics = Run-BenchmarkTask "current" "Current Spring" $SPRING_BIN $false $INPUT_ABS $maxReadLen
$v1Metrics = Run-BenchmarkTask "spring_v1" "Spring v1" $mambaBin $true $INPUT_ABS $maxReadLen

Print-Report $currentMetrics
Print-Report $v1Metrics
Print-Comparison $currentMetrics $v1Metrics
