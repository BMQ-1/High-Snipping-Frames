@echo off
echo =======================================================
echo  Building High Snipping Frames (x64 Release)
echo =======================================================

where g++ >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo Error: MinGW g++ not found in PATH.
    pause
    exit /b 1
)

if not exist "..\bin" mkdir "..\bin"

echo [1/3] Compiling SnippingToolHighFPS.dll...
g++ -O2 -shared -o ..\bin\SnippingToolHighFPS.dll ..\src\SnippingToolHighFPS.cpp -lole32 -luuid -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolHighFPS.dll
    exit /b 1
)

echo [2/3] Compiling SnippingToolDaemon.exe...
g++ -O2 -mwindows -municode -o ..\bin\SnippingToolDaemon.exe ..\src\SnippingToolDaemon.cpp -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolDaemon.exe
    exit /b 1
)

echo [3/3] Compiling SnippingToolLauncher.exe...
g++ -O2 -mwindows -municode -o ..\bin\SnippingToolLauncher.exe ..\src\SnippingToolLauncher.cpp -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolLauncher.exe
    exit /b 1
)

echo.
echo Build successful! Binaries are located in ..\bin\
