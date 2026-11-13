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

echo [1/3] Compiling embedded resources and application icon...
windres ..\src\resource.rc -O coff -o ..\src\dll_res.res
if %ERRORLEVEL% neq 0 (
    echo Failed to compile DLL resources.
    exit /b 1
)
windres -DBUILD_GUI ..\src\resource.rc -O coff -o ..\src\gui_res.res
if %ERRORLEVEL% neq 0 (
    echo Failed to compile GUI resources.
    exit /b 1
)

echo [2/3] Compiling SnippingToolHighFPS.dll (In-Memory Kernel Patcher)...
g++ -O2 -shared -o ..\bin\SnippingToolHighFPS.dll ..\src\SnippingToolHighFPS.cpp ..\src\dll_res.res -lole32 -luuid -lgdi32 -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build SnippingToolHighFPS.dll
    exit /b 1
)

echo [3/3] Compiling HighSnippingFramesGUI.exe (Unified Control Center, Tray ^& Watcher)...
g++ -O2 -mwindows -municode -o ..\bin\HighSnippingFramesGUI.exe ..\src\HighSnippingFramesGUI.cpp ..\src\gui_res.res -lgdiplus -ldwmapi -lshell32 -lole32 -ladvapi32 -static -s
if %ERRORLEVEL% neq 0 (
    echo Failed to build HighSnippingFramesGUI.exe
    exit /b 1
)
del /q ..\src\dll_res.res ..\src\gui_res.res 2>nul
if %ERRORLEVEL% neq 0 (
    echo Failed to build HighSnippingFramesGUI.exe
    exit /b 1
)

echo.
echo =======================================================
echo  Build successful! Core binaries generated in ..\bin\
echo =======================================================
