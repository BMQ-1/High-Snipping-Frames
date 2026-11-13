# High Snipping Frames - Installer Script v1.2
# Installs unified Control Center GUI (with integrated zero-CPU injector & real-time clipboard engine)

$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "       High Snipping Frames - Installer v1.2           " -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$repoDir = Split-Path -Parent $scriptDir
$binDir = Join-Path $repoDir "bin"

$installDir = "C:\Program Files\SnippingToolHighFPS"
$progDataDir = "C:\ProgramData\SnippingToolHighFPS"
$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not (Test-Path $installDir)) {
    if ($isElevated) {
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    } else {
        $installDir = Join-Path $env:LOCALAPPDATA "HighSnippingFrames"
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    }
}

if (-not (Test-Path $progDataDir)) {
    New-Item -ItemType Directory -Path $progDataDir -Force | Out-Null
}

Write-Host "[1/6] Stopping any running instances..." -ForegroundColor Yellow
Stop-Process -Name "SnippingToolDaemon", "HighSnippingFramesGUI", "SnippingTool" -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 600

Write-Host "[2/6] Deploying unified binaries..." -ForegroundColor Yellow
$filesToDeploy = @(
    "SnippingToolHighFPS.dll",
    "HighSnippingFramesGUI.exe"
)

foreach ($f in $filesToDeploy) {
    Copy-Item (Join-Path $binDir $f) -Destination (Join-Path $installDir $f) -Force
}

# Remove any legacy v1.0/v1.1 executables or shadowing INI from Program Files
foreach ($legacy in @("SnippingToolHighFPS.ini", "SnippingToolDaemon.exe", "SnippingToolLauncher.exe")) {
    $legacyPath = Join-Path $installDir $legacy
    if (Test-Path $legacyPath) { Remove-Item $legacyPath -Force -ErrorAction SilentlyContinue }
}

# Deploy writable default INI to ProgramData if not present
if (-not (Test-Path (Join-Path $progDataDir "SnippingToolHighFPS.ini"))) {
    Copy-Item (Join-Path $binDir "SnippingToolHighFPS.ini") -Destination (Join-Path $progDataDir "SnippingToolHighFPS.ini") -Force
}
# Ensure standard users and UWP AppContainer can read/write ProgramData runtime config & patch status
icacls $progDataDir /grant "BUILTIN\Users:(OI)(CI)M" /grant "*S-1-15-2-1:(OI)(CI)RX" /T /Q | Out-Null

Write-Host "[3/6] Purging legacy IFEO hooks to ensure instant shortcuts..." -ForegroundColor Yellow
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

Write-Host "[4/6] Configuring security and AppContainer permissions..." -ForegroundColor Yellow
icacls (Join-Path $installDir "*") /grant "ALL APPLICATION PACKAGES:(OI)(CI)RX" /t 2>$null | Out-Null
icacls $progDataDir /grant "ALL APPLICATION PACKAGES:(OI)(CI)RX" /t 2>$null | Out-Null
icacls $progDataDir /grant "BUILTIN\Users:(OI)(CI)M" /t 2>$null | Out-Null

$packageLocalState = Join-Path $env:LOCALAPPDATA "Packages\Microsoft.ScreenSketch_8wekyb3d8bbwe\LocalState"
if (Test-Path $packageLocalState) {
    icacls $packageLocalState /grant "ALL APPLICATION PACKAGES:(OI)(CI)M" /t 2>$null | Out-Null
}

Write-Host "[5/6] Creating Start Menu shortcut..." -ForegroundColor Yellow
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

Write-Host "[6/6] Configuring auto-start in HKCU Run..." -ForegroundColor Yellow
$guiExe = Join-Path $installDir "HighSnippingFramesGUI.exe"

# Clean up legacy dual-process startup entries
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "SnippingToolHighFPSDaemon" -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFramesGUI" -ErrorAction SilentlyContinue

# Register unified single-instance Control Center
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "HighSnippingFrames" -Value ('"' + $guiExe + '" --minimized')

# Launch unified Control Center detached from script
([wmiclass]"win32_process").Create('"' + $guiExe + '" --minimized') | Out-Null

Write-Host "`nHigh Snipping Frames v1.2 installed and launched successfully!" -ForegroundColor Green
Write-Host "Unified engine is active in your System Tray and Start Menu." -ForegroundColor Green
