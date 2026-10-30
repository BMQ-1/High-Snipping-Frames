@echo off
setlocal
cd /d "%~dp0"

echo =======================================================
echo          High Snipping Frames - Uninstaller           
echo =======================================================
echo.

:: Check for Administrator elevation
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting Administrator privileges to uninstall...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Run the core PowerShell uninstaller with bypassed execution policy
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall.ps1"

if %errorlevel% equ 0 (
    echo.
    echo =======================================================
    echo  [SUCCESS] Uninstalled successfully.
    echo  Windows Snipping Tool has been restored to default.
    echo =======================================================
) else (
    echo.
    echo [ERROR] Uninstallation encountered an issue.
)

echo.
pause
