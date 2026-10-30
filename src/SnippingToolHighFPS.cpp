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
static BOOL   g_EnableCompositorUnlock = TRUE;
static BOOL   g_EnableEncoderUnlock = TRUE;
static BOOL   g_BypassThrottle = TRUE;
static WCHAR  g_LogPath[MAX_PATH] = { 0 };

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

    int configuredFps = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 0, iniPath);
    if (configuredFps <= 0) {
        g_TargetFPS = DetectPrimaryRefreshRate();
        Log("[SnippingToolHighFPS] TargetFPS set to Auto (detected display refresh: %u Hz)", g_TargetFPS);
    } else {
        g_TargetFPS = (UINT32)configuredFps;
        Log("[SnippingToolHighFPS] TargetFPS loaded from INI: %u FPS", g_TargetFPS);
    }

    g_EnableCompositorUnlock = GetPrivateProfileIntW(L"Settings", L"UnlockCompositor", 1, iniPath);
    g_EnableEncoderUnlock    = GetPrivateProfileIntW(L"Settings", L"UnlockEncoder", 1, iniPath);
    g_BypassThrottle         = GetPrivateProfileIntW(L"Settings", L"BypassThrottle", 1, iniPath);

    Log("[SnippingToolHighFPS] Active Config -> TargetFPS: %u, CompositorUnlock: %d, EncoderUnlock: %d, BypassThrottle: %d",
        g_TargetFPS, g_EnableCompositorUnlock, g_EnableEncoderUnlock, g_BypassThrottle);
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

        INT64 interval_100ns = (g_TargetFPS > 0) ? (10000000LL / g_TargetFPS) : 0;
        HRESULT hrPut = putInterval(pSession5, interval_100ns);
        Log("[SnippingToolHighFPS] Compositor MinUpdateInterval(%lld ticks [%.2f ms]) -> 0x%08X",
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
        HRESULT hr1 = putNumerator(pRatio, g_TargetFPS);
        HRESULT hr2 = putDenominator(pRatio, 1);
        Log("[SnippingToolHighFPS] IMediaRatio::put_Numerator(%u) -> 0x%08X, put_Denominator(1) -> 0x%08X",
            g_TargetFPS, hr1, hr2);
    }

    return g_TargetFPS;
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
    }
    return TRUE;
}
