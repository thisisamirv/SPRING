#!/usr/bin/env pwsh
# Minimal test case to isolate CRC threading issue

$ErrorActionPreference = "Stop"
$SPRING_BIN = ".\out\build\spring2.exe"
$INPUT_R1 = ".\tests\fixtures\input\SRR8185389_1.fastq.gz"
$INPUT_R2 = ".\tests\fixtures\input\SRR8185389_2.fastq.gz"
$OUTPUT_DIR = ".\out\tests\bench\thread_test"

# Create output directory
New-Item -ItemType Directory -Force -Path $OUTPUT_DIR | Out-Null

Write-Host "=== CRC Threading Test ===" -ForegroundColor Cyan
Write-Host "Testing different thread counts with SRR8185389 dataset`n"

$results = @()

foreach ($threads in @(1, 2, 4, 8)) {
    Write-Host "`n--- Testing with $threads thread(s) ---" -ForegroundColor Yellow
    
    $archivePath = "$OUTPUT_DIR\test_t$threads.sp"
    $outPath = "$OUTPUT_DIR\out_t$threads.fastq"
    
    # Clean up previous run
    Remove-Item $archivePath -Force -ErrorAction SilentlyContinue
    Remove-Item $outPath* -Force -ErrorAction SilentlyContinue
    
    # Compress
    Write-Host "  Compressing with $threads threads..." -NoNewline
    $compStart = Get-Date
    $compOutput = & $SPRING_BIN -v debug -c --R1 $INPUT_R1 --R2 $INPUT_R2 -o $archivePath -t $threads 2>&1 | Out-String
    $compEnd = Get-Date
    $compTime = ($compEnd - $compStart).TotalSeconds
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host " FAILED" -ForegroundColor Red
        Write-Host "  Error during compression"
        continue
    }
    Write-Host " OK ($([math]::Round($compTime, 1))s)" -ForegroundColor Green
    
    # Extract CRC values from compression output
    if ($compOutput -match "sequence_crc_1=(\d+)") { $seqCrc1Comp = $matches[1] } else { $seqCrc1Comp = "N/A" }
    if ($compOutput -match "sequence_crc_2=(\d+)") { $seqCrc2Comp = $matches[1] } else { $seqCrc2Comp = "N/A" }
    if ($compOutput -match "quality_crc_1=(\d+)") { $qualCrc1Comp = $matches[1] } else { $qualCrc1Comp = "N/A" }
    if ($compOutput -match "quality_crc_2=(\d+)") { $qualCrc2Comp = $matches[1] } else { $qualCrc2Comp = "N/A" }
    if ($compOutput -match "id_crc_1=(\d+)") { $idCrc1Comp = $matches[1] } else { $idCrc1Comp = "N/A" }
    if ($compOutput -match "id_crc_2=(\d+)") { $idCrc2Comp = $matches[1] } else { $idCrc2Comp = "N/A" }
    
    Write-Host "  Compression CRCs:"
    Write-Host "    Stream 1: seq=$seqCrc1Comp qual=$qualCrc1Comp id=$idCrc1Comp"
    Write-Host "    Stream 2: seq=$seqCrc2Comp qual=$qualCrc2Comp id=$idCrc2Comp"
    
    # Decompress
    Write-Host "  Decompressing..." -NoNewline
    $decompStart = Get-Date
    $decompOutput = & $SPRING_BIN -v debug -d -i $archivePath -o $outPath 2>&1 | Out-String
    $decompEnd = Get-Date
    $decompTime = ($decompEnd - $decompStart).TotalSeconds
    $decompExitCode = $LASTEXITCODE
    
    if ($decompExitCode -eq 0) {
        Write-Host " OK ($([math]::Round($decompTime, 1))s)" -ForegroundColor Green
        $status = "PASS"
    }
    else {
        Write-Host " FAILED" -ForegroundColor Red
        $status = "FAIL"
        
        # Show which digests mismatched
        if ($decompOutput -match "Stream 1 sequence digest mismatch") { Write-Host "    - Stream 1 sequence mismatch" -ForegroundColor Red }
        if ($decompOutput -match "Stream 1 quality digest mismatch") { Write-Host "    - Stream 1 quality mismatch" -ForegroundColor Red }
        if ($decompOutput -match "Stream 1 ID digest mismatch") { Write-Host "    - Stream 1 ID mismatch" -ForegroundColor Red }
        if ($decompOutput -match "Stream 2 sequence digest mismatch") { Write-Host "    - Stream 2 sequence mismatch" -ForegroundColor Red }
        if ($decompOutput -match "Stream 2 quality digest mismatch") { Write-Host "    - Stream 2 quality mismatch" -ForegroundColor Red }
        if ($decompOutput -match "Stream 2 ID digest mismatch") { Write-Host "    - Stream 2 ID mismatch" -ForegroundColor Red }
    }
    
    $archiveSize = (Get-Item $archivePath).Length / 1MB
    
    $results += [PSCustomObject]@{
        Threads       = $threads
        Status        = $status
        CompTime      = [math]::Round($compTime, 1)
        DecompTime    = [math]::Round($decompTime, 1)
        ArchiveSizeMB = [math]::Round($archiveSize, 2)
        SeqCRC1       = $seqCrc1Comp
        QualCRC1      = $qualCrc1Comp
        IDCRC1        = $idCrc1Comp
        SeqCRC2       = $seqCrc2Comp
        QualCRC2      = $qualCrc2Comp
        IDCRC2        = $idCrc2Comp
    }
}

Write-Host "`n`n=== RESULTS SUMMARY ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize

Write-Host "`n=== CRC COMPARISON ===" -ForegroundColor Cyan
Write-Host "Checking if CRC values are consistent across thread counts..."

$crcFields = @("SeqCRC1", "QualCRC1", "IDCRC1", "SeqCRC2", "QualCRC2", "IDCRC2")
$allSame = $true

foreach ($field in $crcFields) {
    $values = $results | ForEach-Object { $_.$field } | Where-Object { $_ -ne "N/A" } | Select-Object -Unique
    if ($values.Count -gt 1) {
        Write-Host "${field} varies: " -NoNewline -ForegroundColor Red
        Write-Host ($values -join ", ")
        $allSame = $false
    }
    else {
        Write-Host "${field}: $values" -ForegroundColor Green
    }
}

if ($allSame) {
    Write-Host "`nCRC values are CONSISTENT across all thread counts." -ForegroundColor Green
    Write-Host "The issue is in decompression, not compression." -ForegroundColor Yellow
}
else {
    Write-Host "`nCRC values VARY across thread counts!" -ForegroundColor Red
    Write-Host "This indicates a race condition during compression." -ForegroundColor Yellow
}

# Check pass/fail pattern
$passCount = ($results | Where-Object { $_.Status -eq "PASS" }).Count
$failCount = ($results | Where-Object { $_.Status -eq "FAIL" }).Count

Write-Host "`n=== PASS/FAIL PATTERN ===" -ForegroundColor Cyan
Write-Host "Passed: $passCount / $($results.Count)"
Write-Host "Failed: $failCount / $($results.Count)"

if ($failCount -eq 0) {
    Write-Host "`nAll tests PASSED! Issue may be intermittent." -ForegroundColor Green
}
elseif ($passCount -eq 0) {
    Write-Host "`nAll tests FAILED! Issue is systematic." -ForegroundColor Red
}
else {
    Write-Host "`nMixed results. Check if failures correlate with thread count." -ForegroundColor Yellow
    $passingThreads = ($results | Where-Object { $_.Status -eq "PASS" }).Threads
    $failingThreads = ($results | Where-Object { $_.Status -eq "FAIL" }).Threads
    Write-Host "Passing thread counts: $($passingThreads -join ', ')"
    Write-Host "Failing thread counts: $($failingThreads -join ', ')"
}

Write-Host "`n=== TEST COMPLETE ===" -ForegroundColor Cyan
