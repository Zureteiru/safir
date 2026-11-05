# build_switch.ps1 - builds safir.ovl in Docker Desktop, exports artifacts to .\dist and lays
# out the full SD-card package in .\sd_package (both git-ignored).
# Run from this folder:  powershell -ExecutionPolicy Bypass -File .\build_switch.ps1
# Output is a short summary; the full Docker log is in build_switch.log.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$log = Join-Path $PSScriptRoot "build_switch.log"

function Show-Errors {
    # Error lines only, without Docker's echo of whole RUN commands (thousands of characters).
    Select-String -Path $log -Pattern "error|ERROR|FAILED|warning:" |
        Where-Object { $_.Line.Length -lt 400 } | Select-Object -First 25 | ForEach-Object { "  " + $_.Line.Trim() }
}

# Models and JMdict are not in git: fetch them (pinned SHA-256) into .cache\ when missing.
python tools/fetch_assets.py
if ($LASTEXITCODE -ne 0) { Write-Host "== BUILD: FAIL (fetching assets)" -ForegroundColor Red; exit 1 }

Write-Host "building (Docker)..."
cmd /c "docker build --progress=plain -f Dockerfile.switch -t safir . > `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Write-Host "== BUILD: FAIL (docker build). Full log: build_switch.log" -ForegroundColor Red
    Show-Errors
    exit 1
}

$updated = @(docker run --rm -v "${PWD}/dist:/dist" safir 2>$null | Where-Object { $_ -like "updated: *" } |
    ForEach-Object { $_.Substring(9) })
if ($LASTEXITCODE -ne 0) { Write-Host "== BUILD: FAIL (docker run export)" -ForegroundColor Red; exit 1 }
$missing = @("safir.ovl", "safir.elf", "dict.bin", "VOCR6.2_S_w8.vocr", "VOCR6.2_N_w8g2.vocr") |
    Where-Object { -not (Test-Path (Join-Path "dist" $_)) }
if ($missing) { Write-Host "== BUILD: FAIL, missing in dist: $($missing -join ', ')" -ForegroundColor Red; exit 1 }

$logText = Get-Content $log
$bss = ($logText | Select-String "\[CHECK\] Static \.bss size: (\d+)" | Select-Object -Last 1).Matches.Groups[1].Value
$warnings = @($logText | Select-String "warning:" | Where-Object { $_.Line.Length -lt 400 })
# Was the dictionary layer reused? Find its BuildKit step id, then that step's CACHED line.
$dictStep = ($logText | Select-String "^#(\d+) \[.*RUN mkdir -p /workspace/release" | Select-Object -First 1)
$dictState = "rebuilt"
if ($dictStep) {
    $id = $dictStep.Matches.Groups[1].Value
    if ($logText | Select-String "^#$id CACHED") { $dictState = "cached" }
}
$modelOk = (Get-Item dist\VOCR6.2_S_w8.vocr).Length -eq 819792 -and (Get-Item dist\VOCR6.2_N_w8g2.vocr).Length -eq 1189408

$global:LASTEXITCODE = 0
$pkgState = & (Join-Path $PSScriptRoot "tools\make_sd_package.ps1") -Quiet
if ($LASTEXITCODE -ne 0) { Write-Host "== BUILD: FAIL (SD package)" -ForegroundColor Red; exit 1 }

Write-Host "== BUILD: PASS" -ForegroundColor Green
"  .bss        {0:N0} / 1,350,000 B" -f [int]$bss
"  safir.ovl   {0:N0} B" -f (Get-Item dist\safir.ovl).Length
"  warnings    {0}" -f $warnings.Count
"  dict.bin    {0}" -f $dictState
"  updated     {0}" -f $(if ($updated.Count) { $updated -join ", " } else { "nothing (all files unchanged)" })
"  SD package  sd_package\ (copy config\ and switch\ to the SD root), {0}" -f (($pkgState | Select-Object -Last 1) -replace '^package: ', '')
if (-not $modelOk) { Write-Host "  WARNING: models are not the 819,792-byte VOCR6.2_S_w8 and 1,189,408-byte VOCR6.2_N_w8g2" -ForegroundColor Yellow }
if ($warnings.Count -gt 0) { $warnings | Select-Object -First 10 | ForEach-Object { "    " + $_.Line.Trim() } }
