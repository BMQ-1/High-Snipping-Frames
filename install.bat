@echo off
setlocal
cd /d "%~dp0"

echo =======================================================
echo          High Snipping Frames - 1-Click Setup         
echo =======================================================
echo.

:: Check for Administrator elevation
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting Administrator privileges for system installation...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: Run the core PowerShell installer with bypassed execution policy
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1"

if %errorlevel% equ 0 (
    echo.
    echo =======================================================
    echo  [SUCCESS] High Snipping Frames is active!
    echo  - Controls: Double-click the tray icon near your clock.
    echo  - Recording: Open Snipping Tool (or press Win+Shift+S)
    echo    and record at 60 / 120 / 144+ FPS natively!
    echo =======================================================
) else (
    echo.
    echo [ERROR] Installation encountered an issue. See output above.
)

echo.
pause
