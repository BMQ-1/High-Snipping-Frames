@echo off
setlocal
echo =======================================================
echo  Building High Snipping Frames (x64 Release Suite)
echo =======================================================

where g++ >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo Error: MinGW g++ not found in PATH.
    pause
    exit /b 1
)

where windres >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo Error: windres not found in PATH.
    pause
    exit /b 1
)

if not exist "..\bin" mkdir "..\bin"

echo [1/5] Compiling resources and icon...
windres ..\src\resource.rc -O coff -o ..\src\resource.res
if %ERRORLEVEL% neq 0 (
    echo Failed to compile resources.
    exit /b 1
)

echo [2/5] Compiling SnippingToolHighFPS.dll...
g++ -O2 -shared -o ..\bin\SnippingToolHighFPS.dll ..\src\SnippingToolHighFPS.cpp ..\src\resource.res -lole32 -luuid -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolHighFPS.dll
    exit /b 1
)

echo [3/5] Compiling SnippingToolDaemon.exe...
g++ -O2 -mwindows -municode -o ..\bin\SnippingToolDaemon.exe ..\src\SnippingToolDaemon.cpp ..\src\resource.res -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolDaemon.exe
    exit /b 1
)

echo [4/5] Compiling SnippingToolLauncher.exe...
g++ -O2 -mwindows -municode -o ..\bin\SnippingToolLauncher.exe ..\src\SnippingToolLauncher.cpp ..\src\resource.res -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolLauncher.exe
    exit /b 1
)

echo [5/5] Compiling HighSnippingFramesGUI.exe (Control Center & Tray App)...
g++ -O2 -mwindows -municode -o ..\bin\HighSnippingFramesGUI.exe ..\src\HighSnippingFramesGUI.cpp ..\src\resource.res -lcomctl32 -ldwmapi -lshell32 -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build HighSnippingFramesGUI.exe
    exit /b 1
)

echo.
echo =======================================================
echo  Build successful! All 4 binaries created in ..\bin\
echo =======================================================
