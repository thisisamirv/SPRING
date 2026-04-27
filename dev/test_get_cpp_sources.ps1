# dev/test_get_cpp_sources.ps1 - helper to verify Get-CppSources output
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
$target = '.\\vendor\\pthash'
Write-Host "Target: $target"
$resolved = Resolve-RepoPath $target
Write-Host "Resolved: $resolved"
$files = Get-CppSources $target
Write-Host "Found $($files.Count) files"
foreach ($f in $files) { Write-Host $f }
