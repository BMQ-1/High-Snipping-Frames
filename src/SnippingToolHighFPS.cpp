#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// IGraphicsCaptureSession5 GUID: 67c0ea62-1f85-5061-925a-239be0ac09cb
static const IID IID_IGraphicsCaptureSession5 = 
    { 0x67c0ea62, 0x1f85, 0x5061, { 0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb } };

// Active configuration
static UINT32 g_TargetFPS = 144;
static int    g_BitrateMultiplier = 1; // 0 = Balanced, 1 = High Quality (1.5x), 2 = Ultra Gaming (2.0x)
static BOOL   g_EnableCompositorUnlock = TRUE;
static BOOL   g_EnableEncoderUnlock = TRUE;
static BOOL   g_BypassThrottle = TRUE;
static BOOL   g_EnableRecordingHUD = TRUE;
static BOOL   g_PerMonitorAutoMatch = TRUE;
static WCHAR  g_LogPath[MAX_PATH] = { 0 };

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
        FILE* fp = _wfopen(g_LogPath, L"a");
        if (fp) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(fp, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
            fclose(fp);
        }
    }
}

static HMODULE g_hMod = NULL;

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
    return (maxFreq >= 60) ? maxFreq : 144;
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
                    return dm.dmDisplayFrequency;
                }
            }
        }
    }
    return DetectPrimaryRefreshRate();
}

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
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            // Double buffering
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBm = (HBITMAP)SelectObject(memDC, memBm);

            // Dark rounded pill background
            HBRUSH brBg = CreateSolidBrush(RGB(22, 22, 22));
            HBRUSH brOld = (HBRUSH)SelectObject(memDC, brBg);
            HPEN penBorder = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
            HPEN penOld = (HPEN)SelectObject(memDC, penBorder);

            RoundRect(memDC, rc.left, rc.top, rc.right, rc.bottom, 12, 12);

            // Red recording dot
            HBRUSH brRed = CreateSolidBrush(RGB(255, 59, 48));
            SelectObject(memDC, brRed);
            HPEN penRed = CreatePen(PS_SOLID, 1, RGB(255, 59, 48));
            SelectObject(memDC, penRed);
            Ellipse(memDC, 10, (rc.bottom - 8) / 2, 18, (rc.bottom + 8) / 2);

            // Text: REC  144 FPS  |  00:12
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(240, 240, 240));
            HFONT hFont = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

            WCHAR hudText[64];
            DWORD mins = g_HudElapsedSec / 60;
            DWORD secs = g_HudElapsedSec % 60;
            swprintf(hudText, 64, L"REC  %u FPS  |  %02u:%02u", g_HudFPS, mins, secs);

            RECT rcText = { 24, 0, rc.right - 6, rc.bottom };
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

    int w = 156;
    int h = 30;
    int x = mi.rcMonitor.right - w - 24;
    int y = mi.rcMonitor.top + 18;

    g_hWndHUD = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"SnippingToolHighFPS_HUD",
        L"HighFPS_HUD",
        WS_POPUP | WS_VISIBLE,
        x, y, w, h,
        NULL, NULL, g_hMod, NULL
    );

    if (g_hWndHUD) {
        // The magic: EXCLUDE completely from all video recording/captures!
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

static void HideRecordingHUD() {
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

// Load INI configuration dynamically
static void LoadConfig(HMODULE hMod) {
    if (hMod) g_hMod = hMod;
    if (!hMod) hMod = g_hMod;

    WCHAR iniPath[MAX_PATH] = { 0 };
    if (hMod) {
        GetModuleFileNameW(hMod, iniPath, MAX_PATH);
        WCHAR* dot = wcsrchr(iniPath, L'.');
        if (dot) wcscpy(dot, L".ini");
    }

    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(iniPath, L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolHighFPS.ini");
    }
    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(iniPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\High-Snipping-Frames\\bin\\SnippingToolHighFPS.ini");
    }

    // Write log to AppData Local Temp (always writable by packaged apps)
    WCHAR tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath)) {
        swprintf(g_LogPath, MAX_PATH, L"%sSnippingToolHighFPS.log", tempPath);
    }

    g_PerMonitorAutoMatch = GetPrivateProfileIntW(L"Settings", L"PerMonitorAutoMatch", 1, iniPath);
    g_EnableRecordingHUD  = GetPrivateProfileIntW(L"Settings", L"EnableRecordingHUD", 1, iniPath);

    int configuredFps = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 0, iniPath);
    if (configuredFps <= 0) {
        if (g_PerMonitorAutoMatch) {
            g_TargetFPS = DetectActiveMonitorRefreshRate();
            Log("[SnippingToolHighFPS] Per-Monitor Auto Match: TargetFPS set to %u Hz (active monitor)", g_TargetFPS);
        } else {
            g_TargetFPS = DetectPrimaryRefreshRate();
            Log("[SnippingToolHighFPS] Global Auto Match: TargetFPS set to %u Hz (highest display)", g_TargetFPS);
        }
    } else {
        g_TargetFPS = (UINT32)configuredFps;
        Log("[SnippingToolHighFPS] TargetFPS loaded from INI: %u FPS", g_TargetFPS);
    }

    g_BitrateMultiplier      = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 1, iniPath);
    g_EnableCompositorUnlock = GetPrivateProfileIntW(L"Settings", L"UnlockCompositor", 1, iniPath);
    g_EnableEncoderUnlock    = GetPrivateProfileIntW(L"Settings", L"UnlockEncoder", 1, iniPath);
    g_BypassThrottle         = GetPrivateProfileIntW(L"Settings", L"BypassThrottle", 1, iniPath);

    Log("[SnippingToolHighFPS] Active Config -> TargetFPS: %u, BitrateMultiplier: %d, CompositorUnlock: %d, EncoderUnlock: %d, BypassThrottle: %d",
        g_TargetFPS, g_BitrateMultiplier, g_EnableCompositorUnlock, g_EnableEncoderUnlock, g_BypassThrottle);
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

        // Dynamic V-Sync Cadence Pacing:
        // Set exact 100ns presentation interval without integer truncation.
        // During GPU spikes or frame drops, compositor delivers frames immediately.
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

    // Show Invisible In-Recording HUD (excluded from capture via WDA_EXCLUDEFROMCAPTURE)
    ShowRecordingHUD(g_TargetFPS);

    // 1. Unlock compositor to TargetFPS
    UnlockCaptureSession(pSession);

    // 2. Call original StartCapture on pSession (slot 6: rax + 0x30)
    typedef HRESULT (__stdcall *StartCaptureFn)(void*);
    StartCaptureFn startCapture = (StartCaptureFn)(*(void***)pSession)[6];
    HRESULT hr = startCapture(pSession);
    Log("[SnippingToolHighFPS] Original StartCapture returned 0x%08X", hr);
    return hr;
}

// Global replacement for SnippingTool IMediaRatio frame rate query
extern "C" __declspec(dllexport) UINT32 MediaRatioHook(void** ppRatio) {
    // Reload config so any recent GUI/INI adjustments take effect immediately
    LoadConfig(g_hMod);

    if (!ppRatio || !*ppRatio) {
        Log("[SnippingToolHighFPS] MediaRatioHook: NULL ratio pointer, returning %u", g_TargetFPS);
        return g_TargetFPS;
    }

    void* pRatio = *ppRatio;
    Log("[SnippingToolHighFPS] MediaRatioHook called on pRatio=0x%p, setting to %u FPS", pRatio, g_TargetFPS);

    void** vtbl = *(void***)pRatio;
    if (vtbl) {
        typedef HRESULT (__stdcall *PutRatioFn)(void*, UINT32);
        PutRatioFn putNumerator   = (PutRatioFn)vtbl[6];
        PutRatioFn putDenominator = (PutRatioFn)vtbl[8];

        // 1. Audio-Video Drift Prevention: Exact Rational Frame-Rate
        UINT32 num = g_TargetFPS;
        UINT32 den = 1;

        if (num == 59 || num == 60) {
            UINT32 dispFreq = DetectPrimaryRefreshRate();
            if (dispFreq == 59) {
                num = 60000;
                den = 1001; // Exact NTSC 59.940059... fps prevents 1000/1001 audio drift
            }
        }

        HRESULT hr1 = putNumerator(pRatio, num);
        HRESULT hr2 = putDenominator(pRatio, den);
        Log("[SnippingToolHighFPS] IMediaRatio::put_Numerator(%u) -> 0x%08X, put_Denominator(%u) -> 0x%08X (A/V Sync Locked)",
            num, hr1, den, hr2);
    }

    // 2. Hardware Encoder Bitrate Scaling (Game Drop & High-Motion Protection)
    // Snipping Tool multiplies returned value by width * height to allocate encoder bitrate buffer.
    // Scaling this ensures fast-motion scenes never starve the hardware encoder or drop frames.
    UINT32 calcFps = g_TargetFPS;
    if (g_BitrateMultiplier == 1) {
        // High Quality: 1.5x bitrate allocation
        calcFps = (g_TargetFPS * 3) / 2;
    } else if (g_BitrateMultiplier == 2) {
        // Ultra Gaming: 2.0x bitrate allocation
        calcFps = g_TargetFPS * 2;
    }

    Log("[SnippingToolHighFPS] Returning bitrate basis: %u (BitrateMultiplier=%d)", calcFps, g_BitrateMultiplier);
    return calcFps;
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
                Log("[SnippingToolHighFPS] [OK] Patch 2 applied: Frame Interval Math = %u ms at 0x%p", minIntervalMs, pMath);
            }
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 2 pattern not found");
        }
    }

    // -------------------------------------------------------------
    // PATCH 3: IMediaRatio FrameRate Override (0x140085e20)
    // Pattern: c7 44 24 20 af 03 00 00 48 8b 01 48 8b 40 38
    // Function start is at pattern - 0x2f
    // -------------------------------------------------------------
    if (g_EnableEncoderUnlock) {
        const BYTE p3[] = { 0xC7, 0x44, 0x24, 0x20, 0xAF, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x38 };
        BYTE* match3 = FindPattern(textBase, textSize, p3, sizeof(p3));
        if (match3) {
            BYTE* funcStart = match3 - 0x2F;
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
                Log("[SnippingToolHighFPS] [OK] Patch 3 applied: MediaRatioHook placed at 0x%p -> 0x%p", funcStart, (void*)pHook);
            }
        } else {
            Log("[SnippingToolHighFPS] [WARN] Patch 3 pattern not found");
        }
    }

    // -------------------------------------------------------------
    // PATCH 4: StartCapture Compositor Unlock Wrapper Hook
    // Pattern: 48 8b 01 48 8b 08 48 8b 01 48 8b 40 30
    // Wrapper start is at pattern - 0x22 (sub rsp, 0x38)
    // -------------------------------------------------------------
    if (g_EnableCompositorUnlock) {
        const BYTE p4[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x30 };
        BYTE* searchPtr = textBase;
        SIZE_T searchRemaining = textSize;
        int patch4Count = 0;

        while (searchRemaining > sizeof(p4)) {
            BYTE* match4 = FindPattern(searchPtr, searchRemaining, p4, sizeof(p4));
            if (!match4) break;

            BYTE* funcStart = match4 - 0x22;
            DWORD oldProt;
            if (VirtualProtect(funcStart, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR pHook = (ULONG_PTR)StartCaptureHook;

                // 14-byte indirect jump: FF 25 00 00 00 00 <64-bit target>
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
                Log("[SnippingToolHighFPS] [OK] Patch 4 applied: StartCaptureHook placed at 0x%p", funcStart);
            }

            searchRemaining -= (match4 + sizeof(p4) - searchPtr);
            searchPtr = match4 + sizeof(p4);
        }

        if (patch4Count == 0) {
            Log("[SnippingToolHighFPS] [WARN] Patch 4 pattern not found");
        }
    }

    Log("[SnippingToolHighFPS] All patches applied successfully in SnippingTool.exe!");
}

extern "C" __declspec(dllexport) LRESULT CALLBACK MsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        LoadConfig(hinstDLL);
        ApplySnippingToolPatches();
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        HideRecordingHUD();
    }
    return TRUE;
}
