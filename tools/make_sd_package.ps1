# make_sd_package.ps1 - lays out everything safir needs on the SD card, ready to copy in one go:
#   sd_package\switch\.overlays\safir.ovl
#   sd_package\switch\safir\dict.bin, VOCR6.2_S_w8.vocr, VOCR6.2_N_w8g2.vocr
#   sd_package\config\safir\config.ini   (every option)
# Copy the config and switch folders in sd_package onto the SD card root, replacing existing
# files. Only files whose contents changed are rewritten, so timestamps show what is new.
#
# Run from the repo root: powershell -ExecutionPolicy Bypass -File .\tools\make_sd_package.ps1
param([switch]$Quiet) # -Quiet: no file list (build_switch.ps1 prints its own summary)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

foreach ($f in "dist\safir.ovl", "dist\dict.bin", "dist\VOCR6.2_S_w8.vocr", "dist\VOCR6.2_N_w8g2.vocr") {
    if (-not (Test-Path $f)) { Write-Host "MISSING: $f (run build_switch.ps1 first)" -ForegroundColor Red; exit 1 }
}

$pkg = "sd_package"
New-Item -ItemType Directory -Force "$pkg\switch\.overlays", "$pkg\switch\safir", "$pkg\config\safir" | Out-Null
# Files are copied only when their contents differ, so unchanged ones keep their timestamps
# and show what is new.
$script:changed = @()
function Sync-File($src, $dst) {
    if ((Test-Path $dst) -and (Get-Item $src).Length -eq (Get-Item $dst).Length -and
        (Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash) { return }
    Copy-Item -Force $src $dst
    $script:changed += (Split-Path $dst -Leaf)
}
Sync-File dist\safir.ovl "$pkg\switch\.overlays\safir.ovl"
Sync-File dist\dict.bin "$pkg\switch\safir\dict.bin"
Sync-File dist\VOCR6.2_S_w8.vocr "$pkg\switch\safir\VOCR6.2_S_w8.vocr"
Sync-File dist\VOCR6.2_N_w8g2.vocr "$pkg\switch\safir\VOCR6.2_N_w8g2.vocr"

$config = @"
[safir]
; Buttons pressed together to scan the screen (opens/closes safir).
; Names: L R ZL ZR LS RS DUP DDOWN DLEFT DRIGHT A B X Y MINUS PLUS
scan_combo=ZL+ZR
; Popup width in screen pixels: auto (widest that fits in memory) or 480-1024.
; The popup is drawn 1:1 (sharp); 384 px tall. 640 = 0.98 MB, 768 = 1.18 MB.
popup_width=auto
; OCR model: auto (Normal when the overlay memory has room, else Small), small or normal.
; Normal (VOCR6.2_N_w8g2.vocr) makes about half the mistakes and needs ~370 KB more memory.
ocr_model=auto
; 1: keep each scan's screenshot as scan_1..4.jpg in sdmc:/switch/safir/ (for bug reports).
save_scans=0

"@
$cfgPath = Join-Path (Get-Location) "$pkg\config\safir\config.ini"
$cfgText = $config.Replace("`r`n", "`n")
if (-not (Test-Path $cfgPath) -or [IO.File]::ReadAllText($cfgPath) -ne $cfgText) {
    [IO.File]::WriteAllText($cfgPath, $cfgText, (New-Object Text.UTF8Encoding $false))
    $script:changed += "config.ini"
}

$root = (Resolve-Path $pkg).Path
if (Test-Path "$pkg\safir_sd.zip") { Remove-Item -Force "$pkg\safir_sd.zip" } # no zip any more: plain files
if ($script:changed.Count -eq 0) { Write-Output "package: unchanged" }
else { Write-Output ("package: updated " + ($script:changed -join ", ")) }

if (-not $Quiet) {
    Get-ChildItem -Recurse -File -Force $pkg | ForEach-Object {
        "{0,-45} {1,12:N0} bytes" -f $_.FullName.Substring($root.Length + 1), $_.Length
    }
    Write-Host "SD package ready: copy the config and switch folders in $pkg\ to the SD card root." -ForegroundColor Green
}
