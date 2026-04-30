# benchmark/run_lossless_benchmark.ps1 - Lossless performance metrics for SPRING2 on Windows
$ErrorActionPreference = 'Stop'

# Initial paths
$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item (Join-Path $SCRIPT_DIR "..")).FullName
$BUILD_DIR = Join-Path $ROOT_DIR "build"
$SPRING_BIN_NAME = if ($IsWindows) { "spring2.exe" } else { "spring2" }
$RAPIDGZIP_NAME = if ($IsWindows) { "rapidgzip.exe" } else { "rapidgzip" }
$SPRING_BIN_DEFAULT = Join-Path $BUILD_DIR $SPRING_BIN_NAME
$RAPIDGZIP_DEFAULT = Join-Path $BUILD_DIR (Join-Path "indexed_bzip2-build\src\tools" $RAPIDGZIP_NAME)
$SPRING_BIN = if ($env:SPRING_BIN) { $env:SPRING_BIN } else { $SPRING_BIN_DEFAULT }
$SPRING_PREVIEW_BIN = if ($env:SPRING_PREVIEW_BIN) { $env:SPRING_PREVIEW_BIN } else { $SPRING_BIN }
$RAPIDGZIP_BIN = if ($env:RAPIDGZIP_BIN) { $env:RAPIDGZIP_BIN } else { $RAPIDGZIP_DEFAULT }
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

function Expand-GzipToFile($inputPath, $outputPath) {
    if (Test-Path $RAPIDGZIP_BIN) {
        & $RAPIDGZIP_BIN -d -f -o "$outputPath" "$inputPath"
        if ($LASTEXITCODE -ne 0) {
            throw "rapidgzip failed with exit code ${LASTEXITCODE}: $inputPath"
        }
        return
    }

    # Fallback: use built-in .NET stream decompression when rapidgzip is unavailable.
    $input_file = [System.IO.File]::OpenRead($inputPath)
    $gzip = New-Object System.IO.Compression.GZipStream($input_file, [System.IO.Compression.CompressionMode]::Decompress)
    $output = [System.IO.File]::Create($outputPath)
    try {
        $gzip.CopyTo($output)
    }
    finally {
        $output.Close()
        $gzip.Close()
        $input_file.Close()
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

function Get-ArchiveAssayLabel($archivePath) {
    if (-not (Test-Path $SPRING_PREVIEW_BIN)) {
        return "unavailable (preview binary missing)"
    }

    try {
        $previewOutput = & $SPRING_PREVIEW_BIN -p $archivePath 2>$null
        foreach ($line in $previewOutput) {
            if ($line -match '^Assay Type:\s*(.+)$') {
                return $matches[1].Trim()
            }
        }
    }
    catch {
        return "unavailable (preview failed)"
    }

    return "unavailable (assay not found)"
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
    if ((Test-Path $SPRING_BIN) -and (Test-Path $RAPIDGZIP_BIN)) { return }
    
    Write-Host "Spring binary not found; building..." -ForegroundColor Yellow

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

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    
    Write-Host "Running CMake configure..." -ForegroundColor Gray
    & cmake @cmakeConfigArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
    
    Write-Host "Running CMake build..." -ForegroundColor Gray
    & cmake --build "$BUILD_DIR" --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

    if ($IsWindows) {
        Write-Host "Copying runtime DLLs..." -ForegroundColor Gray
        & cmake --build "$BUILD_DIR" --target copy_runtime_dlls
    }
    
    $ErrorActionPreference = $oldPreference
}

# --- Main Logic ---

function Invoke-SingleFileBenchmark($inputPath) {
    $INPUT_ABS = (Get-Item $inputPath).FullName
    $INPUT_BASENAME = [System.IO.Path]::GetFileName($INPUT_ABS)
    $INPUT_STEM = $INPUT_BASENAME
    if ($INPUT_STEM.EndsWith(".gz")) { $INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension($INPUT_STEM) }
    $INPUT_STEM = [System.IO.Path]::GetFileNameWithoutExtension($INPUT_STEM)

    $WORK_DIR = (Join-Path $WORK_ROOT_DIR "$INPUT_STEM.work").Replace("/", "\")
    $OUTPUT_FILE = (Join-Path $OUTPUT_DIR "$INPUT_STEM.sp").Replace("/", "\")
    $DECOMP_FILE = (Join-Path $OUTPUT_DIR "$INPUT_STEM.roundtrip.fastq").Replace("/", "\")

    Write-Host "`n=== Benchmarking $INPUT_BASENAME ===" -ForegroundColor Yellow
    Write-Host "Analyzing FASTQ..." -ForegroundColor Gray
    Get-MaxReadLength $INPUT_ABS | Out-Null

    if (Test-Path $WORK_DIR) { Remove-Item $WORK_DIR -Recurse -Force }
    New-Item -ItemType Directory -Path $WORK_DIR -Force | Out-Null
    if (Test-Path $OUTPUT_FILE) { Remove-Item $OUTPUT_FILE -Force }
    if (Test-Path $DECOMP_FILE) { Remove-Item $DECOMP_FILE -Force }

    Write-Host "Running Spring lossless compression (auto-assay)" -ForegroundColor Cyan
    $compArgs = "-c --R1 `"$INPUT_ABS`" -o `"$OUTPUT_FILE`" -w `"$WORK_DIR`" -t $THREADS -q lossless --assay auto"
    [void](Invoke-ResourceLoggedProcess $SPRING_BIN $compArgs)
    $actualAssay = Get-ArchiveAssayLabel $OUTPUT_FILE

    Write-Host "Running Spring decompression" -ForegroundColor Cyan
    $decompArgs = "-d -i `"$OUTPUT_FILE`" -o `"$DECOMP_FILE`" -w `"$WORK_DIR`""
    [void](Invoke-ResourceLoggedProcess $SPRING_BIN $decompArgs)

    $inputSize = (Get-Item $INPUT_ABS).Length
    $outputSize = (Get-Item $OUTPUT_FILE).Length
    $reduction = if ($inputSize -gt 0) { ($inputSize - $outputSize) * 100 / $inputSize } else { 0 }

    Write-Host "Verifying integrity..." -ForegroundColor Gray
    # For .gz files, we compare against decompressed stream
    $isIdentical = $false
    if ($INPUT_ABS.EndsWith(".gz")) {
        $baseline = Join-Path $WORK_DIR "baseline.fastq"
        Expand-GzipToFile $INPUT_ABS $baseline
        $hash1 = Get-FileHash $baseline -Algorithm SHA256
        $hash2 = Get-FileHash $DECOMP_FILE -Algorithm SHA256
        $isIdentical = ($hash1.Hash -eq $hash2.Hash)
    }
    else {
        $hash1 = Get-FileHash $INPUT_ABS -Algorithm SHA256
        $hash2 = Get-FileHash $DECOMP_FILE -Algorithm SHA256
        $isIdentical = ($hash1.Hash -eq $hash2.Hash)
    }

    Write-Output "  Results for $INPUT_BASENAME"
    Write-Output "    Stored assay:     $actualAssay"
    Write-Output ("    Compressed size: {0:N0} bytes" -f $outputSize)
    Write-Output ("    Reduction:       {0:N2}%" -f $reduction)
    Write-Output ("    Bit-perfect:     $(if ($isIdentical) { 'YES' } else { 'NO' })")
    
    return @{ size = $outputSize; identical = $isIdentical }
}

function Invoke-AssaySuite() {
    Write-Host "`n--- Running Assay Benchmark Suite ---" -ForegroundColor Magenta
    
    $samples = @(
        @{ name = "Bisulfite (test_3)"; r1 = "test_3_R1.fastq.gz"; r2 = "test_3_R2.fastq.gz"; assay = "bisulfite" },
        @{ name = "sc-ATAC (test_4)"; r1 = "test_4_R1.fastq.gz"; r2 = "test_4_R2.fastq.gz"; r3 = "test_4_R3.fastq.gz"; i1 = "test_4_I1.fastq.gz"; assay = "sc-atac" },
        @{ name = "sc-RNA (test_5)"; r1 = "test_5_R1.fastq.gz"; r2 = "test_5_R2.fastq.gz"; i1 = "test_5_I1.fastq.gz"; i2 = "test_5_I2.fastq.gz"; assay = "sc-rna" },
        @{ name = "sc-Bisulfite (test_6)"; r1 = "test_6_R1.fastq.gz"; r2 = "test_6_R2.fastq.gz"; assay = "sc-bisulfite" }
    )

    foreach ($s in $samples) {
        Write-Host "`n>>> Assay: $($s.name)" -ForegroundColor Yellow
        $r1_path = Join-Path $INPUT_DIR $s.r1
        if (-not (Test-Path $r1_path)) { continue }

        $files = @($s.r1)
        if ($s.r2) { $files += $s.r2 }
        if ($s.r3) { $files += $s.r3 }
        if ($s.i1) { $files += $s.i1 }
        if ($s.i2) { $files += $s.i2 }

        $base_args = ""
        $input_abs_paths = @()
        foreach ($f in $files) {
            $abs = (Join-Path $INPUT_DIR $f)
            $input_abs_paths += $abs
            $key = $f.Split("_")[-1].Split(".")[0] # R1, R2, etc.
            $base_args += " --$key `"$abs`""
        }

        $stem = $s.r1.Split(".")[0]
        $out_auto = Join-Path $OUTPUT_DIR "$stem.auto.sp"
        $out_dna = Join-Path $OUTPUT_DIR "$stem.dna.sp"
        $work = Join-Path $WORK_ROOT_DIR "$stem.bench"
        $work_auto = Join-Path $work "auto"
        $work_decomp = Join-Path $work "decomp"
        $work_dna = Join-Path $work "dna"
        
        if (Test-Path $work) { Remove-Item $work -Recurse -Force }
        New-Item -ItemType Directory -Path $work_auto, $work_decomp, $work_dna -Force | Out-Null

        # 1. Auto-detected assay (or explicit for test_6)
        $assay_mode = if ($s.assay -eq "sc-bisulfite") { "sc-bisulfite" } else { "auto" }
        Write-Host "  Step 1: Compression with --assay $assay_mode (expected: $($s.assay))" -ForegroundColor Cyan
        $args_auto = "-c $base_args -o `"$out_auto`" -w `"$work_auto`" -t $THREADS -q lossless --assay $assay_mode"
        [void](Invoke-ResourceLoggedProcess $SPRING_BIN $args_auto)
        $size_auto = (Get-Item $out_auto).Length
        $actual_auto_assay = Get-ArchiveAssayLabel $out_auto
        Write-Host "    Archive metadata assay: $actual_auto_assay" -ForegroundColor Gray

        # 2. Restoration check (Full Round-Trip)
        Write-Host "  Step 2: Verifying bit-perfect restoration..." -ForegroundColor Cyan
        $decomp_dir = Join-Path $work_decomp "restored"
        New-Item -ItemType Directory -Path $decomp_dir -Force | Out-Null
        $decomp_files = @()
        foreach ($f in $files) {
            $decomp_files += Join-Path $decomp_dir ($f.Replace(".gz", ""))
        }
        $o_args = $decomp_files | ForEach-Object { "`"$_`"" }
        $decomp_args = "-d -i `"$out_auto`" -o $($o_args -join ' ') -w `"$work_decomp`""
        [void](Invoke-ResourceLoggedProcess $SPRING_BIN $decomp_args)

        $isIdentical = $true
        for ($i = 0; $i -lt $files.Count; $i++) {
            $orig = $input_abs_paths[$i]
            $restored = $decomp_files[$i]
            
            $baseline = Join-Path $work_decomp "baseline_$i.fastq"
            Expand-GzipToFile $orig $baseline
            $hash1 = Get-FileHash $baseline -Algorithm SHA256
            $hash2 = Get-FileHash $restored -Algorithm SHA256
            if ($hash1.Hash -ne $hash2.Hash) {
                Write-Host "    Mismatch in $($files[$i])!" -ForegroundColor Red
                $isIdentical = $false
            }
        }

        if ($isIdentical) {
            Write-Host "    Bit-perfect: YES" -ForegroundColor Green
        }
        else {
            Write-Host "    Bit-perfect: NO" -ForegroundColor Red
        }

        # 3. DNA-mode comparison
        Write-Host "  Step 3: Compression with --assay dna" -ForegroundColor Cyan
        $args_dna = "-c $base_args -o `"$out_dna`" -w `"$work_dna`" -t $THREADS -q lossless --assay dna"
        [void](Invoke-ResourceLoggedProcess $SPRING_BIN $args_dna)
        $size_dna = (Get-Item $out_dna).Length

        # 4. Results
        $gain = if ($size_dna -gt 0) { ($size_dna - $size_auto) * 100 / $size_dna } else { 0 }
        Write-Output "`n  Assay-specific Optimization Results:"
        Write-Output ("    Expected assay:              {0}" -f $s.assay)
        Write-Output ("    Archive metadata assay:      {0}" -f $actual_auto_assay)
        Write-Output ("    Auto archive size:           {0:N0} bytes" -f $size_auto)
        Write-Output ("    Generic DNA-mode size:       {0:N0} bytes" -f $size_dna)
        Write-Output ("    Optimization Gain:           {0:N2}%" -f $gain)
        
        if ($gain -lt 0) {
            Write-Host "    Warning: Domain optimization was larger than DNA mode!" -ForegroundColor Red
        }
        else {
            Write-Host "    Domain optimization SUCCESS" -ForegroundColor Green
        }
    }
}

Initialize-Environment
Initialize-SpringBinary

if ($args.Count -eq 0 -and (Test-Path (Join-Path $INPUT_DIR "test_3_R1.fastq.gz"))) {
    Invoke-AssaySuite
}
else {
    Invoke-SingleFileBenchmark $INPUT_FILE_ARG
}
