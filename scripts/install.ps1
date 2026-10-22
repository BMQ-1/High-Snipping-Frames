# High Snipping Frames - Installer Script
# Installs background injector, control center GUI, Start Menu shortcut, and autostart

$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Installer v2.0           " -ForegroundColor Cyan
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

Write-Host "[1/5] Stopping running instances..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon", "HighSnippingFramesGUI" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

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

Write-Host "[5/5] Configuring auto-start in HKCU Run..." -ForegroundColor Yellow
$daemonExe = Join-Path $installDir "SnippingToolDaemon.exe"
$guiExe = Join-Path $installDir "HighSnippingFramesGUI.exe"

Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -Value ('"' + $daemonExe + '"')
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFramesGUI" -Value ('"' + $guiExe + '" --minimized')

# Launch daemon using WMI to detach from PowerShell job object
([wmiclass]"win32_process").Create($daemonExe) | Out-Null
([wmiclass]"win32_process").Create($guiExe) | Out-Null

Write-Host "`nHigh Snipping Frames installed and launched successfully!" -ForegroundColor Green
Write-Host "Control Center is now available in your System Tray and Start Menu." -ForegroundColor Green
