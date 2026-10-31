# High Snipping Frames - Installer Script
# Installs unified Control Center GUI (with integrated background watcher), Start Menu shortcut, and autostart

$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Installer v1.0           " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoDir = Split-Path -Parent $scriptDir
$binDir = Join-Path $repoDir "bin"

$installDir = "C:\Program Files\SnippingToolHighFPS"
$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path $installDir)) {
    if ($isElevated) {
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    } else {
        $installDir = Join-Path $env:LOCALAPPDATA "HighSnippingFrames"
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    }
}

Write-Host "[1/5] Stopping any running instances..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon", "HighSnippingFramesGUI", "SnippingTool" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

Write-Host "[2/5] Deploying binaries to $installDir..." -ForegroundColor Yellow
$filesToDeploy = @(
    "SnippingToolHighFPS.dll",
    "SnippingToolDaemon.exe",
    "SnippingToolLauncher.exe",
    "HighSnippingFramesGUI.exe",
    "SnippingToolHighFPS.ini"
)

foreach ($f in $filesToDeploy) {
    Copy-Item (Join-Path $binDir $f) -Destination (Join-Path $installDir $f) -Force
}

Write-Host "[3/5] Granting AppContainer execution permissions..." -ForegroundColor Yellow
icacls (Join-Path $installDir "*") /grant "ALL APPLICATION PACKAGES:(OI)(CI)RX" /t | Out-Null

Write-Host "[4/5] Creating Start Menu shortcut..." -ForegroundColor Yellow
try {
    $wsShell = New-Object -ComObject WScript.Shell
    $startMenuDir = [Environment]::GetFolderPath('Programs')
    $shortcutPath = Join-Path $startMenuDir "High Snipping Frames.lnk"
    $guiExe = Join-Path $installDir "HighSnippingFramesGUI.exe"
    $shortcut = $wsShell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $guiExe
    $shortcut.WorkingDirectory = $installDir
    $shortcut.Description = "High Snipping Frames - Control Center"
    $shortcut.IconLocation = "$guiExe,0"
    $shortcut.Save()
    Write-Host "      Created shortcut at: $shortcutPath" -ForegroundColor Gray
} catch {
    Write-Host "      Could not create shortcut (skipping)" -ForegroundColor Gray
}

Write-Host "[5/5] Configuring unified auto-start in HKCU Run..." -ForegroundColor Yellow
$guiExe = Join-Path $installDir "HighSnippingFramesGUI.exe"

# Clean up legacy dual-process startup entries
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFramesGUI" -ErrorAction SilentlyContinue

# Register unified single-instance Control Center
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFrames" -Value ('"' + $guiExe + '" --minimized')

# Launch unified Control Center detached from script
([wmiclass]"win32_process").Create('"' + $guiExe + '" --minimized') | Out-Null

Write-Host "`nHigh Snipping Frames installed and launched successfully!" -ForegroundColor Green
Write-Host "Unified engine is active in your System Tray and Start Menu (single instance)." -ForegroundColor Green
