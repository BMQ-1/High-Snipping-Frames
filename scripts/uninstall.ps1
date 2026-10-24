# High Snipping Frames - Uninstaller Script

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Uninstaller              " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

Write-Host "[1/4] Stopping processes..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon", "HighSnippingFramesGUI" -Force -ErrorAction SilentlyContinue

Write-Host "[2/4] Removing autostart registry entries..." -ForegroundColor Yellow
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFrames" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFramesGUI" -ErrorAction SilentlyContinue

Write-Host "[3/4] Removing Start Menu shortcut..." -ForegroundColor Yellow
$startMenuDir = [Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenuDir "High Snipping Frames.lnk"
if (Test-Path $shortcutPath) {
    Remove-Item -Path $shortcutPath -Force -ErrorAction SilentlyContinue
}

Write-Host "[4/4] Removing installed files..." -ForegroundColor Yellow
$installDirs = @("C:\Program Files\SnippingToolHighFPS", (Join-Path $env:LOCALAPPDATA "HighSnippingFrames"))
foreach ($dir in $installDirs) {
    if (Test-Path $dir) {
        Remove-Item -Path $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "`nUninstalled successfully. Windows Snipping Tool restored to default." -ForegroundColor Green
