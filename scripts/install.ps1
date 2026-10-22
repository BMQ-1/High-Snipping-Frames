# High Snipping Frames - Installer Script
# Installs background injector and registers user autostart

$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Installer                " -ForegroundColor Cyan
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
        # Fallback to local appdata if not elevated and program files does not exist
        $installDir = Join-Path $env:LOCALAPPDATA "HighSnippingFrames"
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    }
}

Write-Host "[1/4] Terminating any running daemon instances..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon" -Force -ErrorAction SilentlyContinue

Write-Host "[2/4] Deploying binaries to $installDir..." -ForegroundColor Yellow
Copy-Item (Join-Path $binDir "SnippingToolHighFPS.dll") -Destination (Join-Path $installDir "SnippingToolHighFPS.dll") -Force
Copy-Item (Join-Path $binDir "SnippingToolDaemon.exe") -Destination (Join-Path $installDir "SnippingToolDaemon.exe") -Force
Copy-Item (Join-Path $binDir "SnippingToolHighFPS.ini") -Destination (Join-Path $installDir "SnippingToolHighFPS.ini") -Force

Write-Host "[3/4] Granting AppContainer execution permissions..." -ForegroundColor Yellow
icacls (Join-Path $installDir "*") /grant "ALL APPLICATION PACKAGES:(OI)(CI)RX" /t | Out-Null

Write-Host "[4/4] Configuring auto-start in HKCU Run..." -ForegroundColor Yellow
$daemonExe = Join-Path $installDir "SnippingToolDaemon.exe"
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -Value ('"' + $daemonExe + '"')

# Launch daemon using WMI to detach from PowerShell job object
$res = ([wmiclass]"win32_process").Create($daemonExe)
if ($res.ReturnValue -eq 0) {
    Write-Host "`nHigh Snipping Frames installed and running successfully! (PID: $($res.ProcessId))" -ForegroundColor Green
    Write-Host "Snipping Tool video recordings will now capture in High FPS automatically." -ForegroundColor Green
} else {
    Write-Host "`nInstalled successfully, but failed to start daemon (code: $($res.ReturnValue)). Try restarting." -ForegroundColor Red
}
