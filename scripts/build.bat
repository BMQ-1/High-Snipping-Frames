@echo off
setlocal
cd /d "%~dp0\.."

echo [1/4] Checking MinGW-w64 toolchain...
where g++ >nul 2>nul || (echo ERROR: g++ not found in PATH & exit /b 1)
where windres >nul 2>nul || (echo ERROR: windres not found in PATH & exit /b 1)

if not exist bin mkdir bin

echo [2/4] Compiling resources...
windres src\resource.rc -O coff -o src\dll_res.res || exit /b 1
windres src\resource.rc -DBUILD_GUI -O coff -o src\gui_res.res || exit /b 1

echo [3/4] Building SnippingToolHighFPS.dll (v2.0)...
g++ -shared -O2 -Wall -Wextra -fno-exceptions -fno-rtti ^
    src\SnippingToolHighFPS.cpp src\dll_res.res ^
    -o bin\SnippingToolHighFPS.dll ^
    -Wl,--kill-at -static -s
if errorlevel 1 (
    del /q src\dll_res.res src\gui_res.res 2>nul
    exit /b 1
)

echo [4/4] Building HighSnippingFramesGUI.exe (v2.0)...
g++ -O2 -Wall -Wextra -fno-exceptions -fno-rtti -mwindows -municode ^
    src\HighSnippingFramesGUI.cpp src\gui_res.res ^
    -o bin\HighSnippingFramesGUI.exe ^
    -lpsapi -ldwmapi -lshell32 -lole32 -ladvapi32 -static -s
set ERR=%errorlevel%

del /q src\dll_res.res src\gui_res.res 2>nul
if not "%ERR%"=="0" exit /b %ERR%

echo.
echo [SUCCESS] Built v2.0 binaries cleanly in bin\
for %%F in (bin\SnippingToolHighFPS.dll bin\HighSnippingFramesGUI.exe) do echo   %%~nxF : %%~zF bytes
endlocal
