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
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..\..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "out\build"
$SPRING_BIN_NAME = if ($IsWindows) { "spring2.exe" } else { "spring2" }
$SPRING_BIN_DEFAULT = Join-Path $BUILD_DIR $SPRING_BIN_NAME
$SPRING_BIN = if ($env:SPRING_BIN) { $env:SPRING_BIN } else { $SPRING_BIN_DEFAULT }
$THREADS = if ($env:THREADS) { $env:THREADS } else { 8 }
$NO_DEBUG = $false

foreach ($arg in $args) {
    if ($arg -ieq '--no_debug') {
        $NO_DEBUG = $true
    }
    else {
        throw "Unknown argument: $arg"
    }
}

$SPRING_VERBOSE_ARGS = if ($NO_DEBUG) { @() } else { @('-v', 'debug') }

$TMP_DIR = Join-Path $ROOT_DIR "out\tests\bench\big"
$TMP_INPUT_DIR = Join-Path $ROOT_DIR "tests\fixtures\input"
$TMP_LOG_DIR = Join-Path $TMP_DIR "logs"
$TMP_OUTPUT_DIR = Join-Path $TMP_DIR "runs"
$BIG_BENCH_LOG = Join-Path $TMP_LOG_DIR "big_bench.log"

function Write-BigBenchLine {
    param(
        [string]$Message,
        [ConsoleColor]$Color = [ConsoleColor]::White
    )

    Write-Host $Message -ForegroundColor $Color
    Add-Content -Path $BIG_BENCH_LOG -Value $Message
}

function Write-BigBenchRawText {
    param([string]$Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return
    }

    [Console]::Out.Write($Text)
    Add-Content -Path $BIG_BENCH_LOG -Value $Text
}

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
            Write-BigBenchLine "Using existing file: $(Split-Path $path -Leaf)" Gray
            return 
        }
        Write-BigBenchLine "Existing file $(Split-Path $path -Leaf) is too small, re-downloading..." Yellow
        Remove-Item $path -Force
    }
    
    Write-BigBenchLine "Downloading $(Split-Path $path -Leaf)..." Cyan
    Write-BigBenchLine "From: $url" Gray
    
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
    $argumentList = if ($arguments -is [System.Array]) { $arguments } else { @($arguments) }
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $binary
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    foreach ($argument in $argumentList) {
        $null = $psi.ArgumentList.Add([string]$argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $null = $process.Start()
    $stdoutTask = $process.StandardOutput.ReadLineAsync()
    $stderrTask = $process.StandardError.ReadLineAsync()
    $peakRss = 0
    while (-not $process.HasExited -or $stdoutTask -or $stderrTask) {
        $activeTasks = New-Object System.Collections.Generic.List[System.Threading.Tasks.Task]
        if ($stdoutTask) { $activeTasks.Add($stdoutTask) }
        if ($stderrTask) { $activeTasks.Add($stderrTask) }
        if ($activeTasks.Count -gt 0) {
            [void][System.Threading.Tasks.Task]::WaitAny($activeTasks.ToArray(), 500)
        }

        foreach ($pendingTaskInfo in @(
                @{ Task = $stdoutTask; Stream = 'stdout' },
                @{ Task = $stderrTask; Stream = 'stderr' })) {
            if ($null -eq $pendingTaskInfo.Task -or -not $pendingTaskInfo.Task.IsCompleted) {
                continue
            }

            $line = $pendingTaskInfo.Task.Result
            if ($null -ne $line) {
                Write-BigBenchLine $line
                if ($pendingTaskInfo.Stream -eq 'stdout') {
                    $stdoutTask = $process.StandardOutput.ReadLineAsync()
                }
                else {
                    $stderrTask = $process.StandardError.ReadLineAsync()
                }
            }
            else {
                if ($pendingTaskInfo.Stream -eq 'stdout') {
                    $stdoutTask = $null
                }
                else {
                    $stderrTask = $null
                }
            }
        }

        try {
            if (-not $process.HasExited) {
                $currentRss = $process.PeakWorkingSet64
            }
            else {
                $currentRss = $peakRss
            }
            if ($currentRss -gt $peakRss) { $peakRss = $currentRss }
        }
        catch {}
    }
    $process.WaitForExit()
    $sw.Stop()
    if ($process.ExitCode -ne 0) {
        throw "Process failed with exit code $($process.ExitCode): $binary $($argumentList -join ' ')"
    }

    $result = @{}
    $result.elapsed_seconds = $sw.Elapsed.TotalSeconds
    $result.user_seconds = $process.UserProcessorTime.TotalSeconds
    $result.system_seconds = $process.PrivilegedProcessorTime.TotalSeconds
    $result.max_rss_kb = [Math]::Round($peakRss / 1024)
    $result.cpu_percent = "{0:P0}" -f (($result.user_seconds + $result.system_seconds) / $result.elapsed_seconds)
    return $result
}

function Show-StepTimingSummary {
    param([string]$LogPath)

    if (-not (Test-Path $LogPath)) {
        Write-BigBenchLine ""
        Write-BigBenchLine "Step timings"
        Write-BigBenchLine "  No step timings found."
        return
    }

    $pendingStep = $null
    $stepIndex = 0
    $summary = New-Object System.Collections.Generic.List[string]
    foreach ($line in Get-Content $LogPath) {
        if ($line -match '^\s*(.+?) \.\.\.\s*$') {
            $pendingStep = $matches[1].Trim()
            continue
        }

        if ($line -match '^\s*Time for this step:\s*(.+?)\s*$' -and $pendingStep) {
            $stepIndex++
            $summary.Add(("  {0:D2}. {1}: {2}" -f $stepIndex, $pendingStep, $matches[1].Trim()))
            $pendingStep = $null
            continue
        }

        if ($line -match '^\s*Total time for (compression|decompression):\s*(.+?)\s*$') {
            $stepIndex++
            $summary.Add(("  {0:D2}. Total time for {1}: {2}" -f $stepIndex, $matches[1], $matches[2].Trim()))
        }
    }

    Write-BigBenchLine ""
    Write-BigBenchLine "Step timings"
    if ($summary.Count -eq 0) {
        Write-BigBenchLine "  No step timings found."
        return
    }

    $summary | ForEach-Object { Write-BigBenchLine $_ }
}

# Ensure environment
function Initialize-BenchmarkEnv {
    New-Item -ItemType Directory -Path $TMP_INPUT_DIR, $TMP_LOG_DIR, $TMP_OUTPUT_DIR -Force | Out-Null
    if (Test-Path $BIG_BENCH_LOG) { Remove-Item $BIG_BENCH_LOG -Force }
    New-Item -ItemType File -Path $BIG_BENCH_LOG -Force | Out-Null
    
    Invoke-BenchmarkDataDownload $URL_R1 $PATH_R1
    Invoke-BenchmarkDataDownload $URL_R2 $PATH_R2

    # SETUP PATHS
    $global:INPUT_STEM = "SRR8185389_pe"
    $global:OUTPUT_FILE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
    $global:DECOMP_BASE = (Join-Path $TMP_OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")
    
    $global:DECOMP_FILE_1 = "$global:DECOMP_BASE.1"
    $global:DECOMP_FILE_2 = "$global:DECOMP_BASE.2"
}

function Initialize-SpringBinary {
    if (Test-Path $SPRING_BIN) { return }
    Write-BigBenchLine "Spring binary not found; building..." Yellow
    $buildLog = Join-Path $TMP_LOG_DIR "build.log"
    $env:CC = "gcc"; $env:CXX = "g++"
    $cmakeConfigArgs = @("-S", "$ROOT_DIR", "-B", "$BUILD_DIR", "-G", "Ninja", "-DSPRING_STATIC_RUNTIMES=OFF")
    & cmake @cmakeConfigArgs 2>&1 | Set-Content $buildLog
    & cmake --build "$BUILD_DIR" --parallel 2>&1 | Add-Content $buildLog
    if ($IsWindows) { & cmake --build "$BUILD_DIR" --target copy_runtime_dlls 2>&1 | Add-Content $buildLog }
}

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


# --- Main Logic ---
Initialize-BenchmarkEnv
Initialize-SpringBinary

# Cleanup previous
foreach ($f in @($global:OUTPUT_FILE, $global:DECOMP_FILE_1, $global:DECOMP_FILE_2)) {
    if (Test-Path $f) { Remove-Item $f -Force }
}

# --- Compression ---
Write-BigBenchLine "Running Spring paired-end compression (SRR2990433)" Cyan
Write-BigBenchLine "  R1:      $PATH_R1"
Write-BigBenchLine "  R2:      $PATH_R2"
Write-BigBenchLine "  threads: $THREADS"

$compArgs = @($SPRING_VERBOSE_ARGS + @('-c', '--R1', $PATH_R1, '--R2', $PATH_R2, '-o', $global:OUTPUT_FILE, '-t', $THREADS, '-q', 'lossless', '-n', 'Big Benchmark SRR2990433'))
$compResults = Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs

# --- Decompression ---
Write-BigBenchLine ""
Write-BigBenchLine "Running Spring decompression" Cyan
$decompArgs = @($SPRING_VERBOSE_ARGS + @('-d', '-i', $global:OUTPUT_FILE, '-o', $global:DECOMP_BASE))
$decompResults = Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs

# --- Results ---
$inputSize = (Get-Item $PATH_R1).Length + (Get-Item $PATH_R2).Length
$outputSize = (Get-Item $global:OUTPUT_FILE).Length
$decompSize = (Get-Item $global:DECOMP_FILE_1).Length + (Get-Item $global:DECOMP_FILE_2).Length

$reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }
$ratio = if ($outputSize -gt 0) { $inputSize / $outputSize } else { 0 }

# Integrity Check
Write-BigBenchLine ""
Write-BigBenchLine "Verifying integrity..." Gray
$hash_orig_1 = Get-DecompHash $PATH_R1
$hash_orig_2 = Get-DecompHash $PATH_R2
$hash_decomp_1 = Get-DecompHash $global:DECOMP_FILE_1
$hash_decomp_2 = Get-DecompHash $global:DECOMP_FILE_2

$status1 = if ($hash_orig_1 -eq $hash_decomp_1) { "match" } else { "mismatch" }
$status2 = if ($hash_orig_2 -eq $hash_decomp_2) { "match" } else { "mismatch" }

# Reporting
Write-BigBenchLine ""
Write-BigBenchLine "Benchmark result (Paired-End combined)"
Write-BigBenchLine ("  original bytes:   {0:N0}" -f $inputSize)
Write-BigBenchLine ("  compressed bytes: {0:N0}" -f $outputSize)
Write-BigBenchLine ("  decompressed bytes: {0:N0}" -f $decompSize)
Write-BigBenchLine ("  compression pass time:    {0:N3}s" -f $compResults.elapsed_seconds)
Write-BigBenchLine ("  decompression pass time:  {0:N3}s" -f $decompResults.elapsed_seconds)
Write-BigBenchLine ("  size reduction:   {0:N2}%" -f $reduction)
Write-BigBenchLine ("  compression ratio {0:N3}x" -f $ratio)

Write-BigBenchLine ""
Write-BigBenchLine "Compression resources"
Write-BigBenchLine ("  elapsed time:     {0:N3}s" -f $compResults.elapsed_seconds)
Write-BigBenchLine ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $compResults.max_rss_kb, ($compResults.max_rss_kb / 1024))

Write-BigBenchLine ""
Write-BigBenchLine "Decompression resources"
Write-BigBenchLine ("  elapsed time:     {0:N3}s" -f $decompResults.elapsed_seconds)
Write-BigBenchLine ("  peak memory:      {0} KB ({1:N2} MB RSS)" -f $decompResults.max_rss_kb, ($decompResults.max_rss_kb / 1024))

Write-BigBenchLine ""
Write-BigBenchLine "Round-trip check"
Write-BigBenchLine "  Read 1 status: $status1"
Write-BigBenchLine "  Read 2 status: $status2"
Write-BigBenchLine "  Overall status: $(if ($status1 -eq 'match' -and $status2 -eq 'match') { 'PASSED' } else { 'FAILED' })"

Show-StepTimingSummary $BIG_BENCH_LOG
