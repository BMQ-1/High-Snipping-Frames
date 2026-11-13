# High Snipping Frames - Uninstaller Script v1.2

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Uninstaller v1.2          " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

Write-Host "[1/5] Stopping processes..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon", "HighSnippingFramesGUI" -Force -ErrorAction SilentlyContinue

Write-Host "[2/5] Removing autostart registry entries..." -ForegroundColor Yellow
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFrames" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFramesGUI" -ErrorAction SilentlyContinue

Write-Host "[3/5] Purging any legacy IFEO hooks..." -ForegroundColor Yellow
$ifeoKeys = @(
    "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\SnippingTool.exe",
    "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ScreenClippingHost.exe",
    "HKCU:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\SnippingTool.exe"
)
foreach ($ik in $ifeoKeys) {
    if (Test-Path $ik) {
        Remove-ItemProperty -Path $ik -Name "Debugger" -ErrorAction SilentlyContinue
    }
}

Write-Host "[4/5] Removing Start Menu shortcut..." -ForegroundColor Yellow
$startMenuDir = [Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenuDir "High Snipping Frames.lnk"
if (Test-Path $shortcutPath) {
    Remove-Item -Path $shortcutPath -Force -ErrorAction SilentlyContinue
}

Write-Host "[5/5] Removing installed files and data..." -ForegroundColor Yellow
$installDirs = @(
    "C:\Program Files\SnippingToolHighFPS",
    "C:\ProgramData\SnippingToolHighFPS",
    (Join-Path $env:LOCALAPPDATA "HighSnippingFrames")
)
foreach ($dir in $installDirs) {
    if (Test-Path $dir) {
        Remove-Item -Path $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "`nUninstalled successfully. Windows Snipping Tool restored to factory default." -ForegroundColor Green
