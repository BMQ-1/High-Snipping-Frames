# High Snipping Frames - Release Packaging Script v1.2
# Generates a standalone, ready-to-publish GitHub Release ZIP

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoDir = Split-Path -Parent $scriptDir
$distDir = Join-Path $repoDir "dist"
$tempStageDir = Join-Path $distDir "High-Snipping-Frames-Portable"
$zipOutput = Join-Path $distDir "High-Snipping-Frames-v1.2-Portable.zip"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Packaging Release v1.2   " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

# Ensure dist directories are fresh
if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }
New-Item -ItemType Directory -Path $tempStageDir -Force | Out-Null

Write-Host "[1/4] Staging binaries..." -ForegroundColor Yellow
$binStage = Join-Path $tempStageDir "bin"
New-Item -ItemType Directory -Path $binStage -Force | Out-Null

$binFiles = @(
    "HighSnippingFramesGUI.exe",
    "SnippingToolHighFPS.dll",
    "SnippingToolHighFPS.ini"
)

foreach ($f in $binFiles) {
    Copy-Item (Join-Path (Join-Path $repoDir "bin") $f) -Destination (Join-Path $binStage $f) -Force
}

Write-Host "[2/4] Staging scripts and documentation..." -ForegroundColor Yellow
$scriptsStage = Join-Path $tempStageDir "scripts"
New-Item -ItemType Directory -Path $scriptsStage -Force | Out-Null
Copy-Item (Join-Path $repoDir "scripts\install.ps1") -Destination (Join-Path $scriptsStage "install.ps1") -Force
Copy-Item (Join-Path $repoDir "scripts\uninstall.ps1") -Destination (Join-Path $scriptsStage "uninstall.ps1") -Force
Copy-Item (Join-Path $repoDir "install.bat") -Destination (Join-Path $tempStageDir "install.bat") -Force
Copy-Item (Join-Path $repoDir "uninstall.bat") -Destination (Join-Path $tempStageDir "uninstall.bat") -Force
Copy-Item (Join-Path $repoDir "LICENSE") -Destination (Join-Path $tempStageDir "LICENSE") -Force

# Quick Start README for the ZIP
$txtReadme = @"
===============================================================
       High Snipping Frames v1.2 - 60 / 120 / 144+ FPS Suite
===============================================================

HOW TO USE (1-CLICK SETUP):
1. Extract this entire zip file to any folder.
2. Double-click 'install.bat'.
   (Click 'Yes' on the standard Windows permission prompt).
3. Done! 

The High Snipping Frames icon is now in your System Tray,
and Windows Snipping Tool will now record in ultra-smooth
high framerates (60, 120, 144+ FPS) natively with optimized,
shareable video file sizes!

PORTABLE MODE:
- You can also run 'bin\HighSnippingFramesGUI.exe' directly
  from any folder without installing!

TO UNINSTALL:
- Double-click 'uninstall.bat' at any time to restore Windows
  Snipping Tool back to factory stock settings.

GitHub Repository:
https://github.com/BMQ-1/High-Snipping-Frames
===============================================================
"@
Set-Content -Path (Join-Path $tempStageDir "QUICK_START.txt") -Value $txtReadme -Encoding UTF8

Write-Host "[3/4] Creating ZIP archive..." -ForegroundColor Yellow
Compress-Archive -Path "$tempStageDir\*" -DestinationPath $zipOutput -CompressionLevel Optimal
Copy-Item (Join-Path (Join-Path $repoDir "bin") "HighSnippingFramesGUI.exe") -Destination (Join-Path $distDir "HighSnippingFramesGUI.exe") -Force

Write-Host "[4/4] Cleaning up staging folder..." -ForegroundColor Yellow
Remove-Item $tempStageDir -Recurse -Force

Write-Host "`n[SUCCESS] Release archive created:" -ForegroundColor Green
Write-Host "          $zipOutput" -ForegroundColor Cyan
Write-Host "          File Size: $((Get-Item $zipOutput).Length / 1KB) KB" -ForegroundColor Cyan
