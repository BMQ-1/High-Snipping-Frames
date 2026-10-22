# High Snipping Frames - Uninstaller Script

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Uninstaller              " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

Write-Host "[1/3] Stopping background daemon..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon" -Force -ErrorAction SilentlyContinue

Write-Host "[2/3] Removing autostart registry entry..." -ForegroundColor Yellow
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -ErrorAction SilentlyContinue

Write-Host "[3/3] Removing installed files..." -ForegroundColor Yellow
$installDirs = @("C:\Program Files\SnippingToolHighFPS", (Join-Path $env:LOCALAPPDATA "HighSnippingFrames"))
foreach ($dir in $installDirs) {
    if (Test-Path $dir) {
        Remove-Item -Path $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "`nUninstalled successfully. Windows Snipping Tool restored to default." -ForegroundColor Green
