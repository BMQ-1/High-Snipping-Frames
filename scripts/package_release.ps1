# Packages High Snipping Frames v2.0 into dist\High-Snipping-Frames-v2.0.zip
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DistDir  = Join-Path $RepoRoot "dist"
$StageDir = Join-Path $DistDir "High-Snipping-Frames-v2.0"
$ZipPath  = Join-Path $DistDir "High-Snipping-Frames-v2.0.zip"

if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
if (Test-Path $ZipPath)  { Remove-Item $ZipPath -Force }

New-Item -ItemType Directory -Path (Join-Path $StageDir "bin") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $StageDir "scripts") -Force | Out-Null

Copy-Item (Join-Path $RepoRoot "bin\HighSnippingFramesGUI.exe") (Join-Path $StageDir "bin\") -Force
Copy-Item (Join-Path $RepoRoot "bin\SnippingToolHighFPS.dll")   (Join-Path $StageDir "bin\") -Force
Copy-Item (Join-Path $RepoRoot "bin\SnippingToolHighFPS.ini")   (Join-Path $StageDir "bin\") -Force

Copy-Item (Join-Path $RepoRoot "scripts\install.ps1")   (Join-Path $StageDir "scripts\") -Force
Copy-Item (Join-Path $RepoRoot "scripts\uninstall.ps1") (Join-Path $StageDir "scripts\") -Force

Copy-Item (Join-Path $RepoRoot "install.bat")   $StageDir -Force
Copy-Item (Join-Path $RepoRoot "uninstall.bat") $StageDir -Force
Copy-Item (Join-Path $RepoRoot "README.md")     $StageDir -Force
if (Test-Path (Join-Path $RepoRoot "LICENSE")) {
    Copy-Item (Join-Path $RepoRoot "LICENSE") $StageDir -Force
}

$quickStart = @"
High Snipping Frames v2.0 — Quick Start
=======================================

Option A: Install Permanently (Recommended)
1. Double-click install.bat (accept the UAC prompt).
2. Done! Runs silently in your System Tray and auto-matches your monitor Hz.

Option B: Portable Mode (No Installation)
1. Open the bin\ folder.
2. Double-click HighSnippingFramesGUI.exe.

Controls:
- Left-click tray icon: Toggle Control Center
- Right-click tray icon: Target Framerate presets, Launch Snipping Tool, Open Recordings Folder, Rescan Processes, Exit
- Ctrl + Alt + R: Quick-launch Snipping Tool (configurable in Control Center)
"@
Set-Content -Path (Join-Path $StageDir "QUICK_START.txt") -Value $quickStart -Encoding UTF8

Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal
Remove-Item $StageDir -Recurse -Force

$sizeKB = [math]::Round((Get-Item $ZipPath).Length / 1KB, 1)
Write-Host "[OK] Packaged: $ZipPath ($sizeKB KB)" -ForegroundColor Green
