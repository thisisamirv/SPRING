$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
$ROOT_DIR = (Get-Item "$SCRIPT_DIR\..").FullName
$ASSET_DIR = Join-Path $ROOT_DIR "assets\sample-data"
$BUILD_DIR_OVERRIDE = $null

for ($i = 0; $i -lt $args.Count; $i++) {
    $arg = [string]$args[$i]
    if ($arg -eq "--build" -or $arg -eq "-b" -or $arg -eq "-build") {
        if ($i + 1 -ge $args.Count) {
            throw "Missing value for $arg"
        }
        $BUILD_DIR_OVERRIDE = [string]$args[$i + 1]
        $i++
        continue
    }

    if ($arg.StartsWith("--build=")) {
        $BUILD_DIR_OVERRIDE = $arg.Substring("--build=".Length)
        continue
    }

    if ($arg -eq "-h" -or $arg -eq "--help") {
        Write-Host "Usage: ./smoke_test.ps1 [--build <path>]"
        exit 0
    }

    throw "Unknown argument: $arg"
}

$BUILD_DIR = if ($BUILD_DIR_OVERRIDE) { $BUILD_DIR_OVERRIDE } elseif ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $ROOT_DIR "build" }
if (-not [System.IO.Path]::IsPathRooted($BUILD_DIR)) {
    $BUILD_DIR = Join-Path $ROOT_DIR $BUILD_DIR
}
$SPRING_BIN = $env:SPRING_BIN
if (-not $SPRING_BIN) { $SPRING_BIN = Join-Path $BUILD_DIR "spring2.exe" }

$SPRING_PREVIEW_BIN = $env:SPRING_PREVIEW_BIN
if (-not $SPRING_PREVIEW_BIN) { $SPRING_PREVIEW_BIN = Join-Path $BUILD_DIR "spring2-preview.exe" }

$SPRING_COMMAND_TIMEOUT_SECONDS = $env:SPRING_COMMAND_TIMEOUT_SECONDS
if (-not $SPRING_COMMAND_TIMEOUT_SECONDS) { $SPRING_COMMAND_TIMEOUT_SECONDS = 0 }

$SMOKE_THREADS_COMPRESS = if ($env:SMOKE_THREADS_COMPRESS) { [int]$env:SMOKE_THREADS_COMPRESS } else { 8 }
$SMOKE_THREADS_DECOMPRESS = if ($env:SMOKE_THREADS_DECOMPRESS) { [int]$env:SMOKE_THREADS_DECOMPRESS } else { 5 }

function Get-DetectedThreadCount {
    $cpuCount = [Environment]::ProcessorCount
    if (-not $cpuCount -or $cpuCount -lt 1) {
        return 1
    }
    return [int]$cpuCount
}

function Get-CappedThreadCount {
    param(
        [int]$Requested,
        [int]$Max
    )

    if ($Requested -lt 1) {
        $Requested = 1
    }
    if ($Max -lt 1) {
        $Max = 1
    }
    if ($Requested -gt $Max) {
        return $Max
    }
    return $Requested
}

$DETECTED_THREADS = Get-DetectedThreadCount
$SMOKE_THREADS_COMPRESS = Get-CappedThreadCount -Requested $SMOKE_THREADS_COMPRESS -Max $DETECTED_THREADS
$SMOKE_THREADS_DECOMPRESS = Get-CappedThreadCount -Requested $SMOKE_THREADS_DECOMPRESS -Max $DETECTED_THREADS

function Ensure-SmokeBinaries {
    if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
        Write-Error "cmake is required to build smoke-test binaries"
        exit 1
    }

    if (-not (Test-Path $BUILD_DIR)) {
        Write-Error "Build directory not found: $BUILD_DIR"
        exit 1
    }

    Write-Host "Building smoke binaries (spring2, spring2-preview)..." -ForegroundColor Cyan
    & cmake --build $BUILD_DIR --target spring2 spring2-preview --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to build smoke-test binaries"
    }
}

# Create a temporary working directory inside tests/output
$tempBase = Join-Path $ROOT_DIR "tests\output\smoke-test."
$uniqueId = [System.Guid]::NewGuid().ToString().Substring(0, 8)
$WORK_DIR = $tempBase + $uniqueId
New-Item -ItemType Directory -Path $WORK_DIR -Force | Out-Null

$CURRENT_SMOKE_CASE = ""

function Write-SmokeDebug {
    param ($exitCode)
    Write-Host "Smoke debug: case=$($CURRENT_SMOKE_CASE) exit_code=$exitCode" -ForegroundColor Yellow
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

function Write-SmokeDiagnostics {
    Write-Host "Smoke diagnostics: system" -ForegroundColor Yellow
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        Write-Host ("Smoke diagnostics: OS={0} Version={1} Build={2}" -f $os.Caption, $os.Version, $os.BuildNumber) -ForegroundColor Yellow
    }
    catch {
        Write-Host "Smoke diagnostics: OS information unavailable" -ForegroundColor Yellow
    }

    Write-Host ("Smoke diagnostics: PowerShell={0}" -f $PSVersionTable.PSVersion) -ForegroundColor Yellow
    Write-Host ("Smoke diagnostics: PSEdition={0}" -f $PSVersionTable.PSEdition) -ForegroundColor Yellow
    Write-Host ("Smoke diagnostics: cwd={0}" -f (Get-Location)) -ForegroundColor Yellow
    Write-Host ("Smoke diagnostics: PATH={0}" -f $env:PATH) -ForegroundColor Yellow

    Write-Host "Smoke diagnostics: selected environment" -ForegroundColor Yellow
    Get-ChildItem Env: |
        Where-Object {
            $_.Name -match '^(CI|GITHUB_|RUNNER_|SPRING_|OMP_|CC$|CXX$|PATH$|SHELL$|LANG$|LC_)'
        } |
        Sort-Object Name |
        ForEach-Object {
            Write-Host ("  {0}={1}" -f $_.Name, $_.Value) -ForegroundColor Yellow
        }

    Write-Host "Smoke diagnostics: tool versions" -ForegroundColor Yellow
    $tools = @('cmake', 'ninja', 'gcc', 'g++', 'clang', 'tar', 'python', 'python3')
    foreach ($tool in $tools) {
        $toolCmd = Get-Command $tool -ErrorAction SilentlyContinue
        if ($toolCmd) {
            try {
                $versionLine = (& $tool --version 2>&1 | Select-Object -First 1)
                Write-Host ("  {0}: {1}" -f $tool, $versionLine) -ForegroundColor Yellow
            }
            catch {
                Write-Host ("  {0}: version query failed" -f $tool) -ForegroundColor Yellow
            }
        }
        else {
            Write-Host ("  {0}: not found" -f $tool) -ForegroundColor Yellow
        }
    }

    if (Test-Path $SPRING_BIN) {
        try {
            $springVersion = (& $SPRING_BIN --version 2>&1 | Select-Object -First 1)
            Write-Host ("  spring2: {0}" -f $springVersion) -ForegroundColor Yellow
        }
        catch {
            Write-Host "  spring2: version query failed" -ForegroundColor Yellow
        }
    }

    if (Test-Path $SPRING_PREVIEW_BIN) {
        try {
            $previewVersion = (& $SPRING_PREVIEW_BIN --version 2>&1 | Select-Object -First 1)
            Write-Host ("  spring2-preview: {0}" -f $previewVersion) -ForegroundColor Yellow
        }
        catch {
            Write-Host "  spring2-preview: version query failed" -ForegroundColor Yellow
        }
    }
}

function Remove-SmokeWorkDir {
    if (Test-Path $WORK_DIR) {
        Remove-Item -Recurse -Force $WORK_DIR -ErrorAction SilentlyContinue
    }
}

function Format-CommandToken {
    param([string]$value)

    if ($null -eq $value) {
        return "''"
    }

    if ($value -match "[\s`"'`$]") {
        return "'" + ($value -replace "'", "''") + "'"
    }

    return $value
}

function Invoke-Spring {
    $exe = $SPRING_BIN
    $cmdArgs = @()
    $providedArgs = $args

    if ($providedArgs.Count -gt 0 -and $providedArgs[0] -notmatch "^-") {
        # First argument doesn't look like a flag (like -c or -d), 
        # assume it's an explicit binary path.
        $exe = $providedArgs[0]
        if ($providedArgs.Count -gt 1) {
            $cmdArgs = $providedArgs[1..($providedArgs.Count - 1)]
        }
    }
    else {
        $cmdArgs = $providedArgs
    }

    if ($env:SPRING_BIN_WRAPPER) {
        $wrapper = $env:SPRING_BIN_WRAPPER.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)
        $fullCmd = @($wrapper[0])
        if ($wrapper.Length -gt 1) { $fullCmd += $wrapper[1..($wrapper.Length - 1)] }
        $fullCmd += $exe
        $exe = $fullCmd[0]
        $cmdArgs = @($fullCmd[1..($fullCmd.Count - 1)]) + $cmdArgs
    }

    if ($env:SPRING_TEST_ARGS -and ($exe -notmatch "preview")) {
        $cmdArgs = @($env:SPRING_TEST_ARGS.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)) + $cmdArgs
    }

    if ($exe -notmatch "preview") {
        $hasVerboseFlag = $cmdArgs -contains "-v" -or $cmdArgs -contains "--verbose"
        if (-not $hasVerboseFlag) {
            $cmdArgs = @("-v", "debug") + $cmdArgs
        }
    }

    $isCompress = $cmdArgs -contains "-c" -or $cmdArgs -contains "--compress"
    if ($isCompress) {
        $outputIndex = [Array]::IndexOf($cmdArgs, "-o")
        if ($outputIndex -ge 0 -and $outputIndex + 1 -lt $cmdArgs.Count) {
            $outputPath = $cmdArgs[$outputIndex + 1]
            if (Test-Path $outputPath) {
                Remove-Item $outputPath -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if (-not $exe) { 
        Write-Error "No Spring binary specified or found."
        return 
    }

    $displayExe = Format-CommandToken $exe
    $displayArgs = @()
    foreach ($arg in $cmdArgs) {
        $displayArgs += (Format-CommandToken $arg)
    }

    $exitCode = 0
    if ($SPRING_COMMAND_TIMEOUT_SECONDS -gt 0 -and (Get-Command "timeout" -ErrorAction SilentlyContinue)) {
        Write-Host "Smoke command: timeout $SPRING_COMMAND_TIMEOUT_SECONDS $displayExe $($displayArgs -join ' ')" -ForegroundColor Gray
        $process = Start-Process -FilePath $exe -ArgumentList $cmdArgs -Wait -NoNewWindow -PassThru
        $exitCode = $process.ExitCode
    }
    else {
        Write-Host "Smoke command: $displayExe $($displayArgs -join ' ')" -ForegroundColor Gray
        & $exe @cmdArgs
        $exitCode = $LASTEXITCODE
    }

    if ($exitCode -ne 0) {
        $script:LAST_SPRING_EXIT_CODE = $exitCode
        throw "Spring failed with exit code $exitCode"
    }
}

function Write-SmokeCase {
    param ($caseName)
    $script:CURRENT_SMOKE_CASE = $caseName
    Write-Host "Smoke case: $caseName" -ForegroundColor Cyan
}



function Compare-Files {
    param ($leftPath, $rightPath)

    if (-not (Test-Path $leftPath)) {
        Write-Host "Left comparison file does not exist: $leftPath" -ForegroundColor Red
        Write-Host "Contents of $(Get-Location):" -ForegroundColor Red
        Get-ChildItem | ForEach-Object { Write-Host "  $($_.Name) ($($_.Length) bytes)" -ForegroundColor Red }
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

    $l1 = [System.IO.File]::ReadAllLines((Get-Item $leftPath).FullName).Count
    $l2 = [System.IO.File]::ReadAllLines((Get-Item $rightPath).FullName).Count

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
    Ensure-SmokeBinaries

    if (-not (Test-Path $SPRING_BIN)) {
        Write-Error "Expected built binary at $SPRING_BIN"
        exit 1
    }


    Push-Location $WORK_DIR

    Write-SmokeCase "fastq round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "fasta round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" --R2 "$ASSET_DIR\test_2.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fasta" --R2 "$ASSET_DIR\test_2.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "fasta round-trip repeat"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fasta" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fasta")) { exit 1 }

    Write-SmokeCase "paired fastq round-trip repeat"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" --R2 "$ASSET_DIR\test_2.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq.gz" --R2 "$ASSET_DIR\test_2.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "paired fasta gzipped input round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fasta.gz" --R2 "$ASSET_DIR\test_2.fasta.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fasta")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fasta")) { exit 1 }

    Write-SmokeCase "single gzipped fastq round-trip"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "paired fastq gzipped input round-trip redundant"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq.gz" --R2 "$ASSET_DIR\test_2.fastq.gz" -o abcd
    Invoke-Spring -d -i abcd -o tmp -u
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "multi-threaded round-trip single"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" -o abcd -t $SMOKE_THREADS_COMPRESS
    Invoke-Spring -d -i abcd -o tmp -t $SMOKE_THREADS_DECOMPRESS
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-SmokeCase "multi-threaded round-trip paired"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" --R2 "$ASSET_DIR\test_2.fastq" -o abcd -t $SMOKE_THREADS_COMPRESS
    Invoke-Spring -d -i abcd -o tmp -t $SMOKE_THREADS_DECOMPRESS
    if (-not (Compare-Files "tmp.1" "$ASSET_DIR\test_1.fastq")) { exit 1 }
    if (-not (Compare-Files "tmp.2" "$ASSET_DIR\test_2.fastq")) { exit 1 }

    Write-SmokeCase "sorted output round-trip single"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" -o abcd -s o
    Invoke-Spring -d -i abcd -o tmp
    $ref1 = Get-Content "$ASSET_DIR\test_1.fastq" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $act1 = Get-Content "tmp" | ForEach-Object { $_.Trim() } | Sort-Object -CaseSensitive
    $ref1 | Set-Content "ref.sorted"
    $act1 | Set-Content "tmp.sorted"
    if (-not (Compare-Files "tmp.sorted" "ref.sorted")) { exit 1 }

    Write-SmokeCase "sorted output round-trip paired"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" --R2 "$ASSET_DIR\test_2.fastq" -o abcd -s o -t $SMOKE_THREADS_COMPRESS
    Invoke-Spring -d -i abcd -o tmp -t $SMOKE_THREADS_DECOMPRESS

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
    Invoke-Spring -c --R1 "$ASSET_DIR\sample.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\sample.fastq")) { exit 1 }

    # Memory capping test
    Write-SmokeCase "memory capping test"
    Invoke-Spring -m 0.1 -c --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -m 0.1 -d -i abcd -o tmp
    if (-not (Compare-Files "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Archive Notes & Previewer Test
    Write-SmokeCase "archive notes & previewer validation"
    Invoke-Spring -c --R1 "$ASSET_DIR\test_1.fastq" -n "SMOKE_TEST_NOTE" -o abcd
    $previewOut = "preview.out"
    Invoke-Spring $SPRING_PREVIEW_BIN abcd | Out-File $previewOut
    $previewContent = Get-Content $previewOut -Raw
    if ($previewContent -notmatch "SMOKE_TEST_NOTE") {
        Write-Error "Failed to find custom note in preview tool output"
    }
    if ($previewContent -notmatch "test_1\.fastq") {
        Write-Error "Failed to find original filename in preview tool output"
    }

    # Lossy quality mode: ill_bin
    Write-SmokeCase "lossy mode: ill_bin"
    Invoke-Spring -c -q ill_bin --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Lossy quality mode: qvz
    Write-SmokeCase "lossy mode: qvz"
    Invoke-Spring -c -q qvz 1 --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }
        
    # Lossy quality mode: binary thresholding
    Write-SmokeCase "lossy mode: binary"
    Invoke-Spring -c -q binary 30 40 10 --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Data Stripping: IDs
    Write-SmokeCase "stripping: ids"
    Invoke-Spring -c -s i --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    # Data Stripping: Quality
    Write-SmokeCase "stripping: quality"
    Invoke-Spring -c -s q --R1 "$ASSET_DIR\test_1.fastq" -o abcd
    Invoke-Spring -d -i abcd -o tmp
    if (-not (Compare-Lines "tmp" "$ASSET_DIR\test_1.fastq")) { exit 1 }

    Write-Host "Tests successful!" -ForegroundColor Green

}
catch {
    Write-SmokeDebug $LASTEXITCODE
    Write-SmokeDiagnostics
    throw $_
}
finally {
    Pop-Location
    Remove-SmokeWorkDir
}
