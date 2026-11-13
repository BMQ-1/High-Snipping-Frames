#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef _MSC_VER
#include <intrin.h>
#define GET_RETURN_ADDRESS() _ReturnAddress()
#else
#define GET_RETURN_ADDRESS() __builtin_return_address(0)
#endif

// IGraphicsCaptureSession5 GUID: 67c0ea62-1f85-5061-925a-239be0ac09cb
static const IID IID_IGraphicsCaptureSession5 = 
    { 0x67c0ea62, 0x1f85, 0x5061, { 0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb } };

// Active configuration
static UINT32 g_TargetFPS = 144;
static int    g_BitrateMultiplier = 0; // 0 = Balanced (16-30 Mbps, perfect for Discord/Slack), 1 = High (22-45 Mbps), 2 = Ultra Gaming (30-65 Mbps)
static BOOL   g_EnableCompositorUnlock = TRUE;
static BOOL   g_EnableEncoderUnlock = TRUE;
static BOOL   g_BypassThrottle = TRUE;
static BOOL   g_EnableRecordingHUD = TRUE;
static BOOL   g_PerMonitorAutoMatch = TRUE;
static BOOL   g_EnableMMCSS = TRUE;       // Windows Multimedia Class Scheduler Service
static WCHAR  g_LogPath[MAX_PATH] = { 0 };
static WCHAR  g_StatusPath[MAX_PATH] = { 0 };

// Thread-safety lock for logging, config loading, and MMCSS transitions
static CRITICAL_SECTION g_csLock;
static volatile LONG    g_csInitialized = 0;

// Patch verification bitmask (bits 0..3 correspond to Patches 1..4)
static DWORD  g_PatchStatusMask = 0;

// Stored pointer to Patch 2 for caller discrimination in MediaRatioHook
static BYTE*  g_pMatch2 = NULL;

// -------------------------------------------------------------
// Invisible In-Recording HUD (WDA_EXCLUDEFROMCAPTURE)
// Displays a sleek pill badge visible to the user's eyes,
// but 100% invisible in the recorded MP4 file!
// -------------------------------------------------------------
static HWND   g_hWndHUD = NULL;
static HANDLE g_hHudThread = NULL;
static DWORD  g_HudElapsedSec = 0;
static UINT32 g_HudFPS = 144;
static volatile BOOL g_bHudRunning = FALSE;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA(buf);

    if (g_LogPath[0]) {
        if (g_csInitialized) EnterCriticalSection(&g_csLock);
        FILE* fp = _wfopen(g_LogPath, L"a");
        if (fp) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(fp, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
            fclose(fp);
        }
        if (g_csInitialized) LeaveCriticalSection(&g_csLock);
    }
}

static HMODULE g_hMod = NULL;

static BOOL IsUnderProgramFiles(const WCHAR* path) {
    if (!path || !path[0]) return FALSE;
    const WCHAR* envVars[] = { L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432" };
    for (int i = 0; i < 3; i++) {
        WCHAR pf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(envVars[i], pf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            if (_wcsnicmp(path, pf, len) == 0 && (path[len] == L'\\' || path[len] == L'\0')) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

// Normalize fractional display refresh rates (e.g. 59.94Hz -> 60Hz, 143.98Hz -> 144Hz)
static UINT32 NormalizeRefreshRate(UINT32 hz) {
    if (hz >= 58 && hz <= 62) return 60;
    if (hz >= 73 && hz <= 77) return 75;
    if (hz >= 88 && hz <= 92) return 90;
    if (hz >= 118 && hz <= 122) return 120;
    if (hz >= 142 && hz <= 146) return 144;
    if (hz >= 163 && hz <= 167) return 165;
    if (hz >= 238 && hz <= 242) return 240;
    if (hz >= 358 && hz <= 362) return 360;
    return hz;
}

// Detect highest refresh rate among all active displays
static UINT32 DetectPrimaryRefreshRate() {
    UINT32 maxFreq = 60;
    DISPLAY_DEVICEW dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);

    for (DWORD devNum = 0; EnumDisplayDevicesW(NULL, devNum, &dd, 0); devNum++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        DEVMODEW dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            if (dm.dmDisplayFrequency > maxFreq && dm.dmDisplayFrequency <= 500) {
                maxFreq = dm.dmDisplayFrequency;
            }
        }
    }
    return NormalizeRefreshRate((maxFreq >= 60) ? maxFreq : 144);
}

// Detect refresh rate of the monitor where the cursor / active recording area is located
static UINT32 DetectActiveMonitorRefreshRate() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        MONITORINFOEXW mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            DEVMODEW dm;
            ZeroMemory(&dm, sizeof(dm));
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                if (dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500) {
                    return NormalizeRefreshRate(dm.dmDisplayFrequency);
                }
            }
        }
    }
    return DetectPrimaryRefreshRate();
}

typedef HRESULT (WINAPI *fnGetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);

static float GetMonitorScale(HMONITOR hMon) {
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        fnGetDpiForMonitor pGetDpi = (fnGetDpiForMonitor)GetProcAddress(hShcore, "GetDpiForMonitor");
        if (pGetDpi) {
            UINT dpiX = 96, dpiY = 96;
            if (SUCCEEDED(pGetDpi(hMon, 0, &dpiX, &dpiY)) && dpiY > 0) {
                return (float)dpiY / 96.0f;
            }
        }
    }
    HDC hdcScreen = GetDC(NULL);
    int dpi = GetDeviceCaps(hdcScreen, LOGPIXELSY);
    ReleaseDC(NULL, hdcScreen);
    float s = (dpi > 0) ? ((float)dpi / 96.0f) : 1.0f;
    return (s < 1.0f) ? 1.0f : s;
}

static void HideRecordingHUD();

static LRESULT CALLBACK HudWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            SetTimer(hWnd, 1, 1000, NULL); // 1-second elapsed timer
            break;
        }
        case WM_TIMER: {
            g_HudElapsedSec++;
            InvalidateRect(hWnd, NULL, FALSE);
            break;
        }
        case WM_RBUTTONUP: {
            // Failsafe: User can right-click the HUD to dismiss it immediately
            HideRecordingHUD();
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            float s = GetMonitorScale(hMon);

            // Double buffering
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBm = (HBITMAP)SelectObject(memDC, memBm);

            // Dark rounded pill background
            HBRUSH brBg = CreateSolidBrush(RGB(22, 22, 22));
            HBRUSH brOld = (HBRUSH)SelectObject(memDC, brBg);
            HPEN penBorder = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
            HPEN penOld = (HPEN)SelectObject(memDC, penBorder);

            int roundRadius = (int)(12.0f * s + 0.5f);
            RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, roundRadius, roundRadius);

            // Red recording dot
            int dotSize = (int)(8.0f * s + 0.5f);
            int dotLeft = (int)(10.0f * s + 0.5f);
            int dotTop  = (rc.bottom - dotSize) / 2;
            HBRUSH brRed = CreateSolidBrush(RGB(255, 59, 48));
            SelectObject(memDC, brRed);
            HPEN penRed = CreatePen(PS_SOLID, 1, RGB(255, 59, 48));
            SelectObject(memDC, penRed);
            Ellipse(memDC, dotLeft, dotTop, dotLeft + dotSize, dotTop + dotSize);

            // Text: REC  144 FPS  |  00:12
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(240, 240, 240));
            int fontHeight = - (int)(11.0f * s + 0.5f);
            HFONT hFont = CreateFontW(fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

            WCHAR hudText[64];
            DWORD mins = g_HudElapsedSec / 60;
            DWORD secs = g_HudElapsedSec % 60;
            swprintf(hudText, 64, L"REC  %u FPS  |  %02u:%02u", g_HudFPS, mins, secs);

            RECT rcText = { (LONG)(24.0f * s + 0.5f), 0, rc.right - (LONG)(6.0f * s + 0.5f), rc.bottom };
            DrawTextW(memDC, hudText, -1, &rcText, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            // Copy to screen
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(memDC, oldFont);
            DeleteObject(hFont);
            SelectObject(memDC, brOld);
            DeleteObject(brBg);
            SelectObject(memDC, penOld);
            DeleteObject(penBorder);
            DeleteObject(brRed);
            DeleteObject(penRed);
            SelectObject(memDC, oldBm);
            DeleteObject(memBm);
            DeleteDC(memDC);

            EndPaint(hWnd, &ps);
            break;
        }
        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            break;
        }
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static DWORD WINAPI HudThreadProc(LPVOID lpParam) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HudWndProc;
    wc.hInstance = g_hMod;
    wc.lpszClassName = L"SnippingToolHighFPS_HUD";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    float s = GetMonitorScale(hMon);
    if (s < 1.0f) s = 1.0f;

    int w = (int)(164.0f * s + 0.5f);
    int h = (int)(32.0f * s + 0.5f);
    int x = mi.rcMonitor.right - w - (int)(24.0f * s + 0.5f);
    int y = mi.rcMonitor.top + (int)(18.0f * s + 0.5f);

    g_hWndHUD = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"SnippingToolHighFPS_HUD",
        L"HighFPS_HUD",
        WS_POPUP | WS_VISIBLE,
        x, y, w, h,
        NULL, NULL, g_hMod, NULL
    );

    if (g_hWndHUD) {
        // Exclude completely from all video recording/captures
        SetWindowDisplayAffinity(g_hWndHUD, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(g_hWndHUD, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hWndHUD);
    }

    MSG msg;
    while (g_bHudRunning && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hWndHUD) {
        DestroyWindow(g_hWndHUD);
        g_hWndHUD = NULL;
    }
    UnregisterClassW(L"SnippingToolHighFPS_HUD", g_hMod);
    return 0;
}

static void ShowRecordingHUD(UINT32 targetFps) {
    if (!g_EnableRecordingHUD) return;
    if (g_bHudRunning) return; // already active

    g_HudFPS = targetFps;
    g_HudElapsedSec = 0;
    g_bHudRunning = TRUE;
    g_hHudThread = CreateThread(NULL, 0, HudThreadProc, NULL, 0, NULL);
}

// Windows Multimedia Class Scheduler Service (MMCSS) Dynamic Loader
typedef HANDLE (WINAPI *fnAvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *fnAvRevertMmThreadCharacteristics)(HANDLE);

static HANDLE g_hMmcssTask = NULL;
static DWORD  g_dwMmcssThreadId = 0;

static void EnableMMCSS() {
    if (g_hMmcssTask) return;
    HMODULE hAvrt = LoadLibraryW(L"avrt.dll");
    if (hAvrt) {
        fnAvSetMmThreadCharacteristicsW pSet = 
            (fnAvSetMmThreadCharacteristicsW)GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW");
        if (pSet) {
            DWORD taskIndex = 0;
            g_hMmcssTask = pSet(L"Capture", &taskIndex);
            if (!g_hMmcssTask) {
                taskIndex = 0;
                g_hMmcssTask = pSet(L"Games", &taskIndex);
            }
            if (g_hMmcssTask) {
                g_dwMmcssThreadId = GetCurrentThreadId();
            }
        }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Log("[SnippingToolHighFPS] MMCSS Quantum Boosting enabled (Task: 0x%p, Priority: HIGHEST)", g_hMmcssTask);
}

static void DisableMMCSS() {
    if (g_csInitialized) EnterCriticalSection(&g_csLock);
    HANDLE hTask = g_hMmcssTask;
    DWORD  dwOrigTid = g_dwMmcssThreadId;
    g_hMmcssTask = NULL;
    g_dwMmcssThreadId = 0;
    if (g_csInitialized) LeaveCriticalSection(&g_csLock);

    if (hTask || dwOrigTid != 0) {
        if (dwOrigTid == GetCurrentThreadId()) {
            if (hTask) {
                HMODULE hAvrt = GetModuleHandleW(L"avrt.dll");
                if (hAvrt) {
                    fnAvRevertMmThreadCharacteristics pRevert = 
                        (fnAvRevertMmThreadCharacteristics)GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics");
                    if (pRevert) pRevert(hTask);
                }
            }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        } else if (dwOrigTid != 0) {
            HANDLE hThread = OpenThread(THREAD_SET_INFORMATION, FALSE, dwOrigTid);
            if (hThread) {
                SetThreadPriority(hThread, THREAD_PRIORITY_NORMAL);
                CloseHandle(hThread);
            }
        }
        Log("[SnippingToolHighFPS] MMCSS Quantum Boosting reverted to normal");
    }
}

static void HideRecordingHUD() {
    if (g_EnableMMCSS) {
        DisableMMCSS();
    }
    if (!g_bHudRunning) return;
    g_bHudRunning = FALSE;
    if (g_hWndHUD) {
        PostMessageW(g_hWndHUD, WM_CLOSE, 0, 0);
    }
    if (g_hHudThread) {
        WaitForSingleObject(g_hHudThread, 1000);
        CloseHandle(g_hHudThread);
        g_hHudThread = NULL;
    }
}

static void PublishPatchStatus(DWORD mask, DWORD expectedMask) {
    if (!g_StatusPath[0]) return;
    int applied = 0, expected = 0;
    for (int i = 0; i < 4; i++) {
        if (mask & (1U << i)) applied++;
        if (expectedMask & (1U << i)) expected++;
    }
    WCHAR buf[64];
    swprintf(buf, 64, L"%d", applied);
    WritePrivateProfileStringW(L"Status", L"AppliedPatches", buf, g_StatusPath);
    swprintf(buf, 64, L"%d", expected);
    WritePrivateProfileStringW(L"Status", L"ExpectedPatches", buf, g_StatusPath);
    swprintf(buf, 64, L"%u", GetCurrentProcessId());
    WritePrivateProfileStringW(L"Status", L"SnippingPID", buf, g_StatusPath);
}

// Load INI configuration dynamically
static void LoadConfig(HMODULE hMod) {
    if (g_csInitialized) EnterCriticalSection(&g_csLock);

    if (hMod) g_hMod = hMod;
    if (!hMod) hMod = g_hMod;

    WCHAR dllIniPath[MAX_PATH] = { 0 };
    WCHAR iniPath[MAX_PATH] = { 0 };
    BOOL  isPortable = FALSE;

    if (hMod) {
        GetModuleFileNameW(hMod, dllIniPath, MAX_PATH);
        WCHAR* dot = wcsrchr(dllIniPath, L'.');
        if (dot) wcscpy(dot, L".ini");
        if (!IsUnderProgramFiles(dllIniPath) && GetFileAttributesW(dllIniPath) != INVALID_FILE_ATTRIBUTES) {
            wcscpy(iniPath, dllIniPath);
            isPortable = TRUE;
        }
    }

    // In installed mode (or if portable INI absent), use shared user-writable %PROGRAMDATA% INI
    if (!isPortable) {
        WCHAR progData[MAX_PATH];
        if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH)) {
            WCHAR progDataIni[MAX_PATH];
            swprintf(progDataIni, MAX_PATH, L"%ls\\SnippingToolHighFPS\\SnippingToolHighFPS.ini", progData);
            if (GetFileAttributesW(progDataIni) != INVALID_FILE_ATTRIBUTES) {
                wcscpy(iniPath, progDataIni);
            }
        }
        if (!iniPath[0] && dllIniPath[0] && GetFileAttributesW(dllIniPath) != INVALID_FILE_ATTRIBUTES) {
            wcscpy(iniPath, dllIniPath);
        }
    }

    // Initialize log & status paths (portable beside DLL, or %PROGRAMDATA% / %TEMP%)
    if (!g_LogPath[0]) {
        if (isPortable && dllIniPath[0]) {
            wcscpy(g_LogPath, dllIniPath);
            WCHAR* dot = wcsrchr(g_LogPath, L'.');
            if (dot) wcscpy(dot, L".log");

            wcscpy(g_StatusPath, dllIniPath);
            WCHAR* sl = wcsrchr(g_StatusPath, L'\\');
            if (sl) wcscpy(sl + 1, L"patch_status.ini");
        } else {
            WCHAR progData[MAX_PATH];
            if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH)) {
                swprintf(g_LogPath, MAX_PATH, L"%ls\\SnippingToolHighFPS\\SnippingToolHighFPS.log", progData);
                swprintf(g_StatusPath, MAX_PATH, L"%ls\\SnippingToolHighFPS\\patch_status.ini", progData);
                FILE* tfp = _wfopen(g_LogPath, L"a");
                if (tfp) {
                    fclose(tfp);
                } else {
                    g_LogPath[0] = L'\0';
                    g_StatusPath[0] = L'\0';
                }
            }
        }
        if (!g_LogPath[0]) {
            WCHAR tempPath[MAX_PATH];
            if (GetTempPathW(MAX_PATH, tempPath)) {
                swprintf(g_LogPath, MAX_PATH, L"%lsSnippingToolHighFPS.log", tempPath);
            }
        }
        // Cap log file size at 256 KB on startup to avoid unbounded disk growth
        if (g_LogPath[0]) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(g_LogPath, GetFileExInfoStandard, &fad)) {
                if (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 262144) {
                    FILE* rfp = _wfopen(g_LogPath, L"w");
                    if (rfp) fclose(rfp);
                }
            }
        }
    }

    g_PerMonitorAutoMatch = GetPrivateProfileIntW(L"Settings", L"PerMonitorAutoMatch", 1, iniPath);

    // Support both EnableRecordingHUD and ShowRecordingHUD aliases
    int hudSetting = GetPrivateProfileIntW(L"Settings", L"EnableRecordingHUD", -1, iniPath);
    if (hudSetting == -1) {
        hudSetting = GetPrivateProfileIntW(L"Settings", L"ShowRecordingHUD", 1, iniPath);
    }
    g_EnableRecordingHUD = (hudSetting != 0);

    int configuredFps = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 0, iniPath);
    if (configuredFps <= 0) {
        if (g_PerMonitorAutoMatch) {
            g_TargetFPS = DetectActiveMonitorRefreshRate();
        } else {
            g_TargetFPS = DetectPrimaryRefreshRate();
        }
    } else {
        g_TargetFPS = (UINT32)configuredFps;
    }

    g_EnableMMCSS            = GetPrivateProfileIntW(L"Settings", L"EnableMMCSS", 1, iniPath);
    g_BitrateMultiplier      = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 0, iniPath); // 0 = Balanced default
    g_EnableCompositorUnlock = GetPrivateProfileIntW(L"Settings", L"UnlockCompositor", 1, iniPath);
    g_EnableEncoderUnlock    = GetPrivateProfileIntW(L"Settings", L"UnlockEncoder", 1, iniPath);
    g_BypassThrottle         = GetPrivateProfileIntW(L"Settings", L"BypassThrottle", 1, iniPath);

    UINT32 snapFps = g_TargetFPS;
    int snapMult = g_BitrateMultiplier;
    BOOL snapComp = g_EnableCompositorUnlock;
    BOOL snapEnc = g_EnableEncoderUnlock;
    BOOL snapByp = g_BypassThrottle;
    BOOL snapPerMon = g_PerMonitorAutoMatch;

    if (g_csInitialized) LeaveCriticalSection(&g_csLock);

    Log("[SnippingToolHighFPS] Config (%ls) -> TargetFPS: %u (AutoMatch=%d), Bitrate: %d, CompUnlock: %d, EncUnlock: %d, Bypass: %d",
        iniPath[0] ? iniPath : L"<default>", snapFps, snapPerMon, snapMult, snapComp, snapEnc, snapByp);
}

// Unlocks compositor minimum update interval on GraphicsCaptureSession
static void UnlockCaptureSession(void* pSession) {
    if (!pSession || !g_EnableCompositorUnlock) return;

    IUnknown* pUnk = (IUnknown*)pSession;
    void* pSession5 = NULL;

    HRESULT hr = pUnk->QueryInterface(IID_IGraphicsCaptureSession5, &pSession5);
    if (SUCCEEDED(hr) && pSession5) {
        typedef HRESULT (__stdcall *PutIntervalFn)(void*, INT64);
        PutIntervalFn putInterval = (PutIntervalFn)(*(void***)pSession5)[7];

        // 100ns interval pacing
        INT64 interval_100ns = (g_TargetFPS > 0) ? (10000000LL / g_TargetFPS) : 0;
        HRESULT hrPut = putInterval(pSession5, interval_100ns);
        Log("[SnippingToolHighFPS] Compositor MinUpdateInterval(%lld ticks [%.2f ms]) -> 0x%08X (Pacing Active)",
            interval_100ns, (double)interval_100ns / 10000.0, hrPut);

        ((IUnknown*)pSession5)->Release();
    } else {
        Log("[SnippingToolHighFPS] QueryInterface(IGraphicsCaptureSession5) -> 0x%08X", hr);
    }
}

// Global replacement for SnippingTool StartCapture wrapper
extern "C" __declspec(dllexport) HRESULT StartCaptureHook(void* pWrapper) {
    if (!pWrapper) return S_OK;
    BYTE* pByte = (BYTE*)pWrapper;
    if (pByte[8] == 0) return S_OK;
    pByte[8] = 0;

    void** pObj = *(void***)pWrapper;
    if (!pObj) return S_OK;
    void* pSession = *pObj;
    if (!pSession) return S_OK;

    // Reload config so any recent GUI/INI adjustments take effect immediately
    LoadConfig(g_hMod);

    Log("[SnippingToolHighFPS] StartCaptureHook triggered on session 0x%p (Target: %u FPS)", pSession, g_TargetFPS);

    // 1. Enable MMCSS Thread Quantum Boosting
    if (g_EnableMMCSS) {
        EnableMMCSS();
    }

    // 2. Show Invisible In-Recording HUD (excluded from capture via WDA_EXCLUDEFROMCAPTURE)
    ShowRecordingHUD(g_TargetFPS);

    // 3. Unlock compositor to TargetFPS
    UnlockCaptureSession(pSession);

    // 4. Hook IClosable::Close on capture session so HUD hides automatically when recording stops
    static const IID IID_IClosable_Local = 
        { 0x30d5a829, 0x7fa4, 0x4026, { 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e } };
    typedef HRESULT (__stdcall *CloseFn)(void*);
    static CloseFn s_pOriginalClose = NULL;

    auto HookedClose = [](void* pThis) -> HRESULT {
        Log("[SnippingToolHighFPS] Capture Session Closed -> Auto-dismissing HUD and reverting MMCSS");
        HideRecordingHUD();
        if (s_pOriginalClose) return s_pOriginalClose(pThis);
        return S_OK;
    };

    IUnknown* pUnkSession = (IUnknown*)pSession;
    void* pClosable = NULL;
    if (SUCCEEDED(pUnkSession->QueryInterface(IID_IClosable_Local, &pClosable)) && pClosable) {
        void** vtbl = *(void***)pClosable;
        if (vtbl && vtbl[6] != (void*)(+HookedClose)) {
            DWORD oldProt;
            if (VirtualProtect(&vtbl[6], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
                if (s_pOriginalClose == NULL) {
                    s_pOriginalClose = (CloseFn)vtbl[6];
                }
                vtbl[6] = (void*)(+HookedClose);
                VirtualProtect(&vtbl[6], sizeof(void*), oldProt, &oldProt);
                Log("[SnippingToolHighFPS] IClosable::Close hooked (original: 0x%p)", s_pOriginalClose);
            }
        }
        ((IUnknown*)pClosable)->Release();
    }

    // 5. Call original StartCapture on pSession (slot 6: rax + 0x30)
    typedef HRESULT (__stdcall *StartCaptureFn)(void*);
    StartCaptureFn startCapture = (StartCaptureFn)(*(void***)pSession)[6];
    HRESULT hr = startCapture(pSession);
    Log("[SnippingToolHighFPS] Original StartCapture returned 0x%08X", hr);
    return hr;
}

// -------------------------------------------------------------
// Smart Bitrate Basis Calculation:
// Solves the "TOO BIG VIDEOS" problem and 32-bit integer overflow!
// Snipping Tool hardcodes: Bitrate = Width * Height * returned_fps * 0.14
// By returning a calibrated basis instead of an inflated raw FPS:
// 1. Videos are compact, high quality, and shareable on Discord/Slack.
// 2. At 4K (8.3M pixels), 2 * Width * Height * basis NEVER overflows INT_MAX (2.14B).
// -------------------------------------------------------------
static UINT32 GetBitrateBasis(UINT32 targetFps, int qualityLevel) {
    // QualityLevel:
    // 0 = Balanced (Default): ~16 Mbps at 1080p, ~28 Mbps at 1440p, ~62 Mbps at 4K.
    // 1 = High Quality:       ~22 Mbps at 1080p, ~39 Mbps at 1440p, ~86 Mbps at 4K.
    // 2 = Ultra Gaming:       ~28 Mbps at 1080p, ~50 Mbps at 1440p, ~111 Mbps at 4K.
    UINT32 base = 52;
    if (qualityLevel == 1) base = 72;
    else if (qualityLevel == 2) base = 94;

    // Sub-linear scaling for higher FPS (inter-frame temporal redundancy increases with FPS)
    if (targetFps >= 200) base = (base * 12) / 10;
    else if (targetFps >= 120) base = (base * 11) / 10;

    // Guard against 32-bit signed overflow: 2 * 3840 * 2160 * base < 2,147,483,647 (max basis = 129)
    if (base > 120) base = 120;
    return base;
}

// Global replacement for SnippingTool IMediaRatio frame rate query
extern "C" __declspec(dllexport) UINT32 MediaRatioHook(void** ppRatio) {
    LoadConfig(g_hMod);

    void* pRetAddr = GET_RETURN_ADDRESS();
    BOOL isPacingCaller = FALSE;

    // Dual-caller discrimination:
    // Caller 2 (VideoFrameGenerator::Initialize pacing: 1000 / FPS) is within 64 bytes of match2.
    // Caller 1 (CreateEncodingProfile bitrate: Width * Height * returned_fps * 0.14) is in the profile builder.
    if (g_pMatch2 && ((BYTE*)pRetAddr >= (g_pMatch2 - 64) && (BYTE*)pRetAddr <= (g_pMatch2 + 64))) {
        isPacingCaller = TRUE;
    }

    if (!ppRatio || !*ppRatio) {
        UINT32 retVal = isPacingCaller ? g_TargetFPS : GetBitrateBasis(g_TargetFPS, g_BitrateMultiplier);
        Log("[SnippingToolHighFPS] MediaRatioHook: NULL ratio pointer, returning %u (isPacing=%d)", retVal, isPacingCaller);
        return retVal;
    }

    void* pRatio = *ppRatio;
    void** vtbl = *(void***)pRatio;
    if (vtbl) {
        typedef HRESULT (__stdcall *PutRatioFn)(void*, UINT32);
        PutRatioFn putNumerator   = (PutRatioFn)vtbl[6];
        PutRatioFn putDenominator = (PutRatioFn)vtbl[8];

        // Safe rational timing:
        // Always enforce denominator = 1 so integer division in Caller 2 (num / den) NEVER divides by zero!
        UINT32 num = g_TargetFPS;
        UINT32 den = 1;
        if (num == 59 || num == 60) {
            num = 60;
            den = 1;
        }

        HRESULT hr1 = putNumerator(pRatio, num);
        HRESULT hr2 = putDenominator(pRatio, den);
        Log("[SnippingToolHighFPS] IMediaRatio::put_Numerator(%u) -> 0x%08X, put_Denominator(%u) -> 0x%08X (A/V Sync Locked)",
            num, hr1, den, hr2);
    }

    if (isPacingCaller) {
        Log("[SnippingToolHighFPS] Returning exact TargetFPS to Pacing Caller: %u FPS (1000/%u = %u ms)",
            g_TargetFPS, g_TargetFPS, 1000 / (g_TargetFPS > 0 ? g_TargetFPS : 60));
        return g_TargetFPS;
    }

    UINT32 bitrateBasis = GetBitrateBasis(g_TargetFPS, g_BitrateMultiplier);
    Log("[SnippingToolHighFPS] Returning controlled bitrate basis: %u (BitrateMultiplier=%d, TargetFPS=%u)",
        bitrateBasis, g_BitrateMultiplier, g_TargetFPS);
    return bitrateBasis;
}

// Memory pattern scanner
static BYTE* FindPattern(BYTE* pBase, SIZE_T size, const BYTE* pPattern, SIZE_T patLen) {
    if (size < patLen) return NULL;
    for (SIZE_T i = 0; i <= size - patLen; i++) {
        if (memcmp(&pBase[i], pPattern, patLen) == 0) {
            return &pBase[i];
        }
    }
    return NULL;
}

// Apply runtime binary modifications
static void ApplySnippingToolPatches() {
    HMODULE hExe = GetModuleHandleW(NULL);
    if (!hExe) return;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hExe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hExe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    // Find .text section
    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    BYTE* textBase = NULL;
    SIZE_T textSize = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            textBase = (BYTE*)hExe + sec->VirtualAddress;
            textSize = sec->Misc.VirtualSize;
            break;
        }
    }

    if (!textBase || textSize == 0) {
        Log("[SnippingToolHighFPS] Failed to locate .text section in SnippingTool.exe");
        return;
    }

    Log("[SnippingToolHighFPS] Scanning SnippingTool.exe .text (Base: 0x%p, Size: 0x%IX)...", textBase, textSize);

    DWORD appliedMask = 0;
    DWORD expectedMask = 0;
    if (g_BypassThrottle)         expectedMask |= 0x01;
    if (g_EnableEncoderUnlock)    expectedMask |= 0x06; // Patches 2 & 3
    if (g_EnableCompositorUnlock) expectedMask |= 0x08; // Patch 4

    // -------------------------------------------------------------
    // PATCH 1: Bypass Micro-Jitter Frame Dropper (VideoFrameGenerator::OnFrameArrived)
    // Pattern: 10 27 00 00 48 3b c1 7d 0e -> Change 7d 0e (jge) to eb 0e (jmp)
    // -------------------------------------------------------------
    if (g_BypassThrottle) {
        const BYTE p1[] = { 0x10, 0x27, 0x00, 0x00, 0x48, 0x3B, 0xC1, 0x7D, 0x0E };
        BYTE* match1 = FindPattern(textBase, textSize, p1, sizeof(p1));
        if (match1) {
            BYTE* pJge = match1 + 7;
            DWORD oldProt;
            if (VirtualProtect(pJge, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                pJge[0] = 0xEB; // jmp unconditional
                VirtualProtect(pJge, 2, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pJge, 2);
                appliedMask |= 0x01;
                Log("[SnippingToolHighFPS] [OK] Patch 1 applied: Frame Dropper Bypassed at 0x%p", pJge);
            }
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 1 pattern not found");
        }
    }

    // -------------------------------------------------------------
    // PATCH 2: VideoFrameGenerator 1000/fps Interval Calculation
    // Pattern: 33 d2 b8 e8 03 00 00 f7 f1 -> Change to b8 <interval> 00 00 00 90 90
    // -------------------------------------------------------------
    if (g_EnableEncoderUnlock) {
        const BYTE p2[] = { 0x33, 0xD2, 0xB8, 0xE8, 0x03, 0x00, 0x00, 0xF7, 0xF1 };
        BYTE* match2 = FindPattern(textBase, textSize, p2, sizeof(p2));
        if (match2) {
            g_pMatch2 = match2; // Store for MediaRatioHook caller discrimination
            BYTE* pMath = match2 + 2; // at b8
            BYTE minIntervalMs = (BYTE)((g_TargetFPS > 0) ? (1000 / g_TargetFPS) : 6);
            if (minIntervalMs < 1) minIntervalMs = 1;

            DWORD oldProt;
            if (VirtualProtect(pMath, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
                pMath[0] = 0xB8;
                pMath[1] = minIntervalMs;
                pMath[2] = 0x00;
                pMath[3] = 0x00;
                pMath[4] = 0x00;
                pMath[5] = 0x90; // nop
                pMath[6] = 0x90; // nop
                VirtualProtect(pMath, 7, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pMath, 7);
                appliedMask |= 0x02;
                Log("[SnippingToolHighFPS] [OK] Patch 2 applied: Frame Interval Math = %u ms at 0x%p", minIntervalMs, pMath);
            }
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 2 pattern not found");
        }
    }

    // -------------------------------------------------------------
    // PATCH 3: IMediaRatio FrameRate Override (0x140085e20)
    // Pattern: c7 44 24 20 af 03 00 00 48 8b 01 48 8b 40 38
    // Uses RtlLookupFunctionEntry for 100% resilient function entry resolution!
    // -------------------------------------------------------------
    if (g_EnableEncoderUnlock) {
        const BYTE p3[] = { 0xC7, 0x44, 0x24, 0x20, 0xAF, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x38 };
        BYTE* match3 = FindPattern(textBase, textSize, p3, sizeof(p3));
        if (match3) {
            DWORD64 imgBase = 0;
            PRUNTIME_FUNCTION pRt = RtlLookupFunctionEntry((DWORD64)match3, &imgBase, NULL);
            BYTE* funcStart = pRt ? ((BYTE*)imgBase + pRt->BeginAddress) : (match3 - 0x2F);

            DWORD oldProt;
            if (VirtualProtect(funcStart, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR pHook = (ULONG_PTR)MediaRatioHook;
                BYTE jmp14[14] = {
                    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
                    (BYTE)(pHook & 0xFF),
                    (BYTE)((pHook >> 8) & 0xFF),
                    (BYTE)((pHook >> 16) & 0xFF),
                    (BYTE)((pHook >> 24) & 0xFF),
                    (BYTE)((pHook >> 32) & 0xFF),
                    (BYTE)((pHook >> 40) & 0xFF),
                    (BYTE)((pHook >> 48) & 0xFF),
                    (BYTE)((pHook >> 56) & 0xFF)
                };
                memcpy(funcStart, jmp14, 14);
                VirtualProtect(funcStart, 14, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), funcStart, 14);
                appliedMask |= 0x04;
                Log("[SnippingToolHighFPS] [OK] Patch 3 applied: MediaRatioHook placed at 0x%p (SEH Entry resolved) -> 0x%p", funcStart, (void*)pHook);
            }
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 3 pattern not found");
        }
    }

    // -------------------------------------------------------------
    // PATCH 4: StartCapture Compositor Unlock Wrapper Hook
    // Pattern: 48 8b 01 48 8b 08 48 8b 01 48 8b 40 30
    // Uses RtlLookupFunctionEntry for 100% resilient wrapper entry resolution!
    // -------------------------------------------------------------
    if (g_EnableCompositorUnlock) {
        const BYTE p4[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x30 };
        BYTE* searchPtr = textBase;
        SIZE_T searchRemaining = textSize;
        int patch4Count = 0;

        while (searchRemaining > sizeof(p4)) {
            BYTE* match4 = FindPattern(searchPtr, searchRemaining, p4, sizeof(p4));
            if (!match4) break;

            DWORD64 imgBase = 0;
            PRUNTIME_FUNCTION pRt = RtlLookupFunctionEntry((DWORD64)match4, &imgBase, NULL);
            BYTE* funcStart = pRt ? ((BYTE*)imgBase + pRt->BeginAddress) : (match4 - 0x22);

            DWORD oldProt;
            if (VirtualProtect(funcStart, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR pHook = (ULONG_PTR)StartCaptureHook;

                BYTE jmp14[14] = {
                    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
                    (BYTE)(pHook & 0xFF),
                    (BYTE)((pHook >> 8) & 0xFF),
                    (BYTE)((pHook >> 16) & 0xFF),
                    (BYTE)((pHook >> 24) & 0xFF),
                    (BYTE)((pHook >> 32) & 0xFF),
                    (BYTE)((pHook >> 40) & 0xFF),
                    (BYTE)((pHook >> 48) & 0xFF),
                    (BYTE)((pHook >> 56) & 0xFF)
                };

                memcpy(funcStart, jmp14, 14);
                VirtualProtect(funcStart, 14, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), funcStart, 14);
                patch4Count++;
                Log("[SnippingToolHighFPS] [OK] Patch 4 applied: StartCaptureHook placed at 0x%p (SEH Entry resolved)", funcStart);
            }

            searchRemaining -= (match4 + sizeof(p4) - searchPtr);
            searchPtr = match4 + sizeof(p4);
        }

        if (patch4Count > 0) {
            appliedMask |= 0x08;
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 4 pattern not found");
        }
    }

    g_PatchStatusMask = appliedMask;
    PublishPatchStatus(appliedMask, expectedMask);
    Log("[SnippingToolHighFPS] Patch summary: mask=0x%X (expected=0x%X)", appliedMask, expectedMask);
}

extern "C" __declspec(dllexport) LRESULT CALLBACK MsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_csLock);
        g_csInitialized = 1;
        DisableThreadLibraryCalls(hinstDLL);

        // Pin module in target process so hooked WinRT vtables stay valid for lifetime
        HMODULE hPinned = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)hinstDLL, &hPinned);

        LoadConfig(hinstDLL);
        ApplySnippingToolPatches();
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        HideRecordingHUD();
        DisableMMCSS();
        if (g_csInitialized) {
            g_csInitialized = 0;
            DeleteCriticalSection(&g_csLock);
        }
    }
    return TRUE;
}
