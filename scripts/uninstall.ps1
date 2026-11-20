# High Snipping Frames v2.0 Uninstaller
$ErrorActionPreference = "SilentlyContinue"

Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host "  High Snipping Frames v2.0 - Uninstaller" -ForegroundColor Cyan
Write-Host "=====================================================" -ForegroundColor Cyan

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting Administrator privileges..." -ForegroundColor Yellow
    Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs -Wait
    exit 0
}

Get-Process -Name "HighSnippingFramesGUI" | Stop-Process -Force
Get-Process -Name "SnippingTool" | Stop-Process -Force
Start-Sleep -Milliseconds 300

Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFrames" -Force
Remove-Item -Path (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\High Snipping Frames.lnk") -Force

$ifeoPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\SnippingTool.exe"
if (Test-Path $ifeoPath) { Remove-Item -Path $ifeoPath -Recurse -Force }

$InstallDir = Join-Path $env:ProgramFiles "SnippingToolHighFPS"
$DataDir    = Join-Path $env:ProgramData "SnippingToolHighFPS"
if (Test-Path $InstallDir) { Remove-Item -Path $InstallDir -Recurse -Force }
if (Test-Path $DataDir)    { Remove-Item -Path $DataDir -Recurse -Force }

Write-Host "[OK] Uninstalled cleanly. Windows Snipping Tool is unchanged." -ForegroundColor Green
