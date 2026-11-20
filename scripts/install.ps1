# High Snipping Frames v2.0 Installer
param([switch]$Elevated)

$ErrorActionPreference = "Stop"
$RepoRoot   = Split-Path -Parent $PSScriptRoot
$BinDir     = Join-Path $RepoRoot "bin"
$InstallDir = Join-Path $env:ProgramFiles "SnippingToolHighFPS"
$DataDir    = Join-Path $env:ProgramData "SnippingToolHighFPS"

Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host "  High Snipping Frames v2.0 - Installer" -ForegroundColor Cyan
Write-Host "=====================================================" -ForegroundColor Cyan

# Self-elevate cleanly if needed
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting Administrator privileges to install to Program Files..." -ForegroundColor Yellow
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Elevated"
    Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs -Wait
    exit 0
}

# Stop running instances cleanly before replacing binaries
Get-Process -Name "HighSnippingFramesGUI" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name "SnippingTool" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# Create install and data directories
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null

# Remove any legacy IFEO registry entries from older v1.0 installs
$ifeoPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\SnippingTool.exe"
if (Test-Path $ifeoPath) {
    Remove-Item -Path $ifeoPath -Recurse -Force -ErrorAction SilentlyContinue
}

# Copy binaries
Copy-Item (Join-Path $BinDir "SnippingToolHighFPS.dll")   (Join-Path $InstallDir "SnippingToolHighFPS.dll")   -Force
Copy-Item (Join-Path $BinDir "HighSnippingFramesGUI.exe") (Join-Path $InstallDir "HighSnippingFramesGUI.exe") -Force

$TargetIni = Join-Path $DataDir "SnippingToolHighFPS.ini"
if (-not (Test-Path $TargetIni)) {
    Copy-Item (Join-Path $BinDir "SnippingToolHighFPS.ini") $TargetIni -Force
}
# Remove any legacy INI beside Program Files EXE so ProgramData remains the single source of truth
$LegacyInstallIni = Join-Path $InstallDir "SnippingToolHighFPS.ini"
if (Test-Path $LegacyInstallIni) { Remove-Item $LegacyInstallIni -Force -ErrorAction SilentlyContinue }

# Grant Store AppContainer (*S-1-15-2-1) read/execute on install folder and write on ProgramData logs/status
& icacls $InstallDir /grant "*S-1-15-2-1:(OI)(CI)RX" "Users:(OI)(CI)RX" | Out-Null
& icacls $DataDir    /grant "*S-1-15-2-1:(OI)(CI)M"  "Users:(OI)(CI)M"  | Out-Null

# Create Start Menu shortcut
$StartMenu = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs"
$ShortcutPath = Join-Path $StartMenu "High Snipping Frames.lnk"
$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut($ShortcutPath)
$Shortcut.TargetPath = Join-Path $InstallDir "HighSnippingFramesGUI.exe"
$Shortcut.WorkingDirectory = $InstallDir
$Shortcut.Description = "High Snipping Frames Control Center"
$Shortcut.Save()

# Configure user startup entry (--minimized for silent boot)
$RunKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
New-ItemProperty -Path $RunKey -Name "HighSnippingFrames" -Value "`"$(Join-Path $InstallDir 'HighSnippingFramesGUI.exe')`" --minimized" -PropertyType String -Force | Out-Null

# Launch Control Center silently in the user desktop session
$GuiExe = Join-Path $InstallDir "HighSnippingFramesGUI.exe"
Start-Process -FilePath $GuiExe -ArgumentList "--minimized" -WorkingDirectory $InstallDir

Write-Host ""
Write-Host "[OK] High Snipping Frames v2.0 installed!" -ForegroundColor Green
Write-Host "  - Location: $InstallDir" -ForegroundColor Gray
Write-Host "  - Settings: $TargetIni" -ForegroundColor Gray
Write-Host "  - Running silently in System Tray." -ForegroundColor Gray
