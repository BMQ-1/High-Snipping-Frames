#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define GET_RETURN_ADDRESS() _ReturnAddress()
#else
#define GET_RETURN_ADDRESS() __builtin_return_address(0)
#endif

// Windows.Graphics.Capture.IGraphicsCaptureSession5
// {661E5F51-35AE-4E8E-A352-3305BB912F9B}
static const GUID IID_IGraphicsCaptureSession5 = {
    0x661E5F51, 0x35AE, 0x4E8E, { 0xA3, 0x52, 0x33, 0x05, 0xBB, 0x91, 0x2F, 0x9B }
};

// Windows.Media.MediaProperties.IMediaEncodingProperties
// {B4002AF6-ACD4-4E5A-A24B-5D7498A8B8C4}
static const GUID IID_IMediaEncodingProperties = {
    0xB4002AF6, 0xACD4, 0x4E5A, { 0xA2, 0x4B, 0x5D, 0x74, 0x98, 0xA8, 0xB8, 0xC4 }
};

// Windows.Media.MediaProperties.IVideoEncodingProperties2
// {F743A1EF-D465-4290-A94B-EF0F1528F8E3}
static const GUID IID_IVideoEncodingProperties2 = {
    0xF743A1EF, 0xD465, 0x4290, { 0xA9, 0x4B, 0xEF, 0x0F, 0x15, 0x28, 0xF8, 0xE3 }
};

// Windows.Foundation.IPropertyValueStatics
// {629BDBC8-D932-4FF4-96B9-8D96C5C1E858}
static const GUID IID_IPropertyValueStatics = {
    0x629BDBC8, 0xD932, 0x4FF4, { 0x96, 0xB9, 0x8D, 0x96, 0xC5, 0xC1, 0xE8, 0x58 }
};

// Media Foundation Color Space & Range Attribute GUIDs
static const GUID kMF_MT_YUV_MATRIX           = { 0x3E23D450, 0x2C75, 0x4D25, { 0xA0, 0x0E, 0xB9, 0x16, 0x70, 0xD1, 0x23, 0x27 } };
static const GUID kMF_MT_VIDEO_PRIMARIES      = { 0xDBFBE4D7, 0x0740, 0x4EE0, { 0x81, 0x92, 0x85, 0x0A, 0xB0, 0xE2, 0x19, 0x35 } };
static const GUID kMF_MT_TRANSFER_FUNCTION    = { 0x5FB0FCE9, 0xBE5C, 0x4935, { 0xA8, 0x11, 0xEC, 0x83, 0x8F, 0x8E, 0xED, 0x93 } };
static const GUID kMF_MT_VIDEO_NOMINAL_RANGE  = { 0xC21B8EE5, 0xB956, 0x4071, { 0x8D, 0xAF, 0x32, 0x5E, 0xDF, 0x5C, 0xAB, 0x11 } };

typedef struct { ULONG_PTR reserved[3]; } HSTRING_HEADER_LOCAL;

static HMODULE          g_hMod              = NULL;
static CRITICAL_SECTION g_csLock;
static volatile LONG    g_csReady           = 0;

static WCHAR            g_IniPath[MAX_PATH]    = L"";
static WCHAR            g_LogPath[MAX_PATH]    = L"";
static WCHAR            g_StatusPath[MAX_PATH] = L"";
static FILETIME         g_IniWriteTime         = {};
static BOOL             g_PathsReady           = FALSE;

// Runtime configuration
static UINT32           g_TargetFPS          = 144;
static int              g_ConfiguredFPS      = 0;     // 0 = Auto display match
static BOOL             g_PerMonitorMatch    = TRUE;
static int              g_BitrateMode        = 0;     // 0=Balanced, 1=High, 2=Ultra
static BOOL             g_UnlockCompositor   = TRUE;
static BOOL             g_UnlockEncoder      = TRUE;
static BOOL             g_BypassThrottle     = TRUE;
static BOOL             g_PreventDrops       = TRUE;
static BOOL             g_OptimizeColor      = TRUE;

// Patch tracking
static BYTE*            g_pMatch2            = NULL;
static BYTE*            g_pPatch2Imm         = NULL;  // 4-byte minIntervalMs immediate
static BYTE*            g_pQueueLimitImm     = NULL;  // 4-byte queue overflow limit immediate
static UINT32           g_PatchMask          = 0;
static volatile LONG    g_Patched            = 0;

static void EnsureLock() {
    if (InterlockedCompareExchange(&g_csReady, 1, 0) == 0) {
        InitializeCriticalSection(&g_csLock);
        InterlockedExchange(&g_csReady, 2);
    } else {
        while (InterlockedCompareExchange(&g_csReady, 2, 2) != 2) {
            Sleep(0);
        }
    }
}

static void Log(const char* fmt, ...) {
    if (!g_LogPath[0]) return;

    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[600];
    int len = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    if (len <= 0) return;

    EnsureLock();
    EnterCriticalSection(&g_csLock);
    HANDLE hFile = CreateFileW(g_LogPath, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, line, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
    LeaveCriticalSection(&g_csLock);
}

static UINT32 NormalizeRefreshRate(DWORD freq) {
    switch (freq) {
        case 59:  return 60;
        case 119: return 120;
        case 143: return 144;
        case 164: return 165;
        case 179: return 180;
        case 239: return 240;
        case 299: return 300;
        case 359: return 360;
        default:  return (freq >= 30 && freq <= 500) ? (UINT32)freq : 60;
    }
}

static UINT32 DetectPrimaryRefreshRate() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        return NormalizeRefreshRate(dm.dmDisplayFrequency);
    }
    return 60;
}

static UINT32 DetectActiveMonitorRefreshRate() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) hwnd = GetDesktopWindow();

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hMon) {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
                return NormalizeRefreshRate(dm.dmDisplayFrequency);
            }
        }
    }
    return DetectPrimaryRefreshRate();
}

static void InitPaths(HMODULE hMod) {
    if (g_PathsReady) return;

    WCHAR dllDir[MAX_PATH] = L"";
    if (hMod && GetModuleFileNameW(hMod, dllDir, MAX_PATH)) {
        WCHAR* sl = wcsrchr(dllDir, L'\\');
        if (sl) *(sl + 1) = L'\0';
    }

    WCHAR dllIni[MAX_PATH] = L"";
    WCHAR progDataIni[MAX_PATH] = L"";
    WCHAR progDataDir[MAX_PATH] = L"";

    if (dllDir[0]) {
        _snwprintf(dllIni, MAX_PATH, L"%lsSnippingToolHighFPS.ini", dllDir);
    }

    WCHAR progData[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH)) {
        _snwprintf(progDataDir, MAX_PATH, L"%ls\\SnippingToolHighFPS", progData);
        _snwprintf(progDataIni, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", progDataDir);
    }

    WCHAR pfDir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"ProgramFiles", pfDir, MAX_PATH);
    BOOL isPortable = !(pfDir[0] && _wcsnicmp(dllDir, pfDir, wcslen(pfDir)) == 0);

    // Installed Mode uses %ProgramData% (single source of truth); Portable Mode uses INI beside DLL
    BOOL hasDll = dllIni[0] && (GetFileAttributesW(dllIni) != INVALID_FILE_ATTRIBUTES);
    BOOL hasPd  = progDataIni[0] && (GetFileAttributesW(progDataIni) != INVALID_FILE_ATTRIBUTES);
    if (isPortable) {
        if (hasDll)               wcsncpy(g_IniPath, dllIni, MAX_PATH - 1);
        else if (hasPd)           wcsncpy(g_IniPath, progDataIni, MAX_PATH - 1);
        else if (dllIni[0])       wcsncpy(g_IniPath, dllIni, MAX_PATH - 1);
    } else {
        if (hasPd)                wcsncpy(g_IniPath, progDataIni, MAX_PATH - 1);
        else if (hasDll)          wcsncpy(g_IniPath, dllIni, MAX_PATH - 1);
        else if (progDataIni[0])  wcsncpy(g_IniPath, progDataIni, MAX_PATH - 1);
    }

    // Log & status live in ProgramData when writable, else beside DLL (portable), else TEMP
    BOOL useProgData = FALSE;
    if (progDataDir[0]) {
        WCHAR testLog[MAX_PATH] = L"";
        _snwprintf(testLog, MAX_PATH, L"%ls\\SnippingToolHighFPS.log", progDataDir);
        HANDLE h = CreateFileW(testLog, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            useProgData = TRUE;
            wcsncpy(g_LogPath, testLog, MAX_PATH - 1);
            _snwprintf(g_StatusPath, MAX_PATH, L"%ls\\patch_status.ini", progDataDir);
        }
    }

    if (!useProgData && dllDir[0]) {
        WCHAR testLog[MAX_PATH] = L"";
        _snwprintf(testLog, MAX_PATH, L"%lsSnippingToolHighFPS.log", dllDir);
        HANDLE h = CreateFileW(testLog, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            wcsncpy(g_LogPath, testLog, MAX_PATH - 1);
            _snwprintf(g_StatusPath, MAX_PATH, L"%lspatch_status.ini", dllDir);
        }
    }

    if (!g_LogPath[0]) {
        WCHAR tmp[MAX_PATH] = L"";
        if (GetTempPathW(MAX_PATH, tmp)) {
            _snwprintf(g_LogPath, MAX_PATH, L"%lsSnippingToolHighFPS.log", tmp);
            _snwprintf(g_StatusPath, MAX_PATH, L"%lspatch_status.ini", tmp);
        }
    }

    // Cap log at 64 KB on attach
    if (g_LogPath[0]) {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(g_LogPath, GetFileExInfoStandard, &fad)) {
            if (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 65536) {
                HANDLE h = CreateFileW(g_LogPath, GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        }
    }

    g_PathsReady = TRUE;
}

static void PublishStatus() {
    if (!g_StatusPath[0]) return;

    int coreCount = 0;
    for (int i = 0; i < 4; i++) {
        if (g_PatchMask & (1u << i)) coreCount++;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "[Status]\r\nActiveFPS=%u\r\nBitrateMode=%d\r\nPatchMask=0x%X\r\nPatchCount=%d\r\nPID=%lu\r\nLastUpdate=%02u:%02u:%02u\r\n",
        g_TargetFPS, g_BitrateMode, g_PatchMask, coreCount, GetCurrentProcessId(),
        st.wHour, st.wMinute, st.wSecond);
    if (len <= 0) return;

    HANDLE hFile = CreateFileW(g_StatusPath, GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
}

static UINT32 ComputeMinIntervalMs(UINT32 fps) {
    if (fps == 0) fps = 60;
    // 15% timing headroom avoids integer-truncation frame drops on 143.98 Hz / DWM jitter
    UINT32 ms = 850u / fps;
    return (ms < 1u) ? 1u : ms;
}

static UINT32 ComputeQueueLimitFrames(UINT32 fps) {
    UINT32 limit = fps * 4u; // Preserve ~4 seconds of lossless burst buffering at high FPS
    if (limit < 300u) limit = 300u;
    if (limit > 900u) limit = 900u;
    return limit;
}

static void PatchImm32(BYTE* addr, UINT32 value) {
    if (!addr) return;
    DWORD oldProt = 0;
    if (VirtualProtect(addr, sizeof(UINT32), PAGE_EXECUTE_READWRITE, &oldProt)) {
        memcpy(addr, &value, sizeof(UINT32));
        VirtualProtect(addr, sizeof(UINT32), oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), addr, sizeof(UINT32));
    }
}

static void LoadConfig(BOOL force) {
    InitPaths(g_hMod);

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    BOOL hasIni = g_IniPath[0] && GetFileAttributesExW(g_IniPath, GetFileExInfoStandard, &fad);
    if (!force && hasIni) {
        if (CompareFileTime(&fad.ftLastWriteTime, &g_IniWriteTime) == 0) {
            return;
        }
    }

    EnsureLock();
    EnterCriticalSection(&g_csLock);

    if (hasIni) {
        g_IniWriteTime     = fad.ftLastWriteTime;
        g_PerMonitorMatch  = GetPrivateProfileIntW(L"Settings", L"PerMonitorAutoMatch", 1, g_IniPath) != 0;
        g_ConfiguredFPS    = (int)GetPrivateProfileIntW(L"Settings", L"TargetFPS", 0, g_IniPath);
        g_BitrateMode      = (int)GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 0, g_IniPath);
        g_UnlockCompositor = GetPrivateProfileIntW(L"Settings", L"UnlockCompositor", 1, g_IniPath) != 0;
        g_UnlockEncoder    = GetPrivateProfileIntW(L"Settings", L"UnlockEncoder", 1, g_IniPath) != 0;
        g_BypassThrottle   = GetPrivateProfileIntW(L"Settings", L"BypassThrottle", 1, g_IniPath) != 0;
        g_PreventDrops     = GetPrivateProfileIntW(L"Settings", L"PreventStartupDrops", 1, g_IniPath) != 0;
        g_OptimizeColor    = GetPrivateProfileIntW(L"Settings", L"OptimizeColor", 1, g_IniPath) != 0;
    } else {
        g_PerMonitorMatch  = TRUE;
        g_ConfiguredFPS    = 0;
        g_BitrateMode      = 0;
        g_UnlockCompositor = TRUE;
        g_UnlockEncoder    = TRUE;
        g_BypassThrottle   = TRUE;
        g_PreventDrops     = TRUE;
        g_OptimizeColor    = TRUE;
    }

    if (g_BitrateMode < 0) g_BitrateMode = 0;
    if (g_BitrateMode > 2) g_BitrateMode = 2;

    UINT32 oldFps = g_TargetFPS;
    int oldBitrate = g_BitrateMode;
    if (g_ConfiguredFPS <= 0) {
        g_TargetFPS = g_PerMonitorMatch ? DetectActiveMonitorRefreshRate() : DetectPrimaryRefreshRate();
    } else {
        if (g_ConfiguredFPS < 30)  g_ConfiguredFPS = 30;
        if (g_ConfiguredFPS > 360) g_ConfiguredFPS = 360;
        g_TargetFPS = (UINT32)g_ConfiguredFPS;
    }
    int newBitrate = g_BitrateMode;

    LeaveCriticalSection(&g_csLock);

    if (oldFps != g_TargetFPS) {
        if (g_pPatch2Imm)     PatchImm32(g_pPatch2Imm, ComputeMinIntervalMs(g_TargetFPS));
        if (g_pQueueLimitImm) PatchImm32(g_pQueueLimitImm, ComputeQueueLimitFrames(g_TargetFPS));
    }
    if ((oldFps != g_TargetFPS || oldBitrate != newBitrate) && g_Patched) {
        PublishStatus();
    }
}

static UINT32 GetBitrateBasis(UINT32 targetFps, int qualityLevel) {
    // 0=Balanced (~20M 1080p), 1=High (~27M 1080p), 2=Ultra (~35M 1080p)
    UINT32 base = 64;
    if (qualityLevel == 1)      base = 86;
    else if (qualityLevel == 2) base = 110;

    if (targetFps >= 200)      base = (base * 12) / 10;
    else if (targetFps >= 120) base = (base * 11) / 10;

    if (base > 125) base = 125; // Keep 2 * Width * Height * base < INT_MAX at 4K
    return base;
}

// Helper to write UINT32 MF_MT_* attributes into IMediaEncodingProperties->Properties
static void SetMediaPropUInt32(void* pMap, void* pStatics, const GUID& key, UINT32 val) {
    if (!pMap || !pStatics) return;

    typedef HRESULT (__stdcall *CreateUInt32Fn)(void*, UINT32, IUnknown**);
    typedef HRESULT (__stdcall *MapInsertFn)(void*, const GUID*, IUnknown*, BYTE*);

    CreateUInt32Fn createUInt32 = (CreateUInt32Fn)(*(void***)pStatics)[11];
    MapInsertFn    mapInsert    = (MapInsertFn)(*(void***)pMap)[10];

    IUnknown* pVal = NULL;
    if (SUCCEEDED(createUInt32(pStatics, val, &pVal)) && pVal) {
        BYTE replaced = 0;
        mapInsert(pMap, &key, pVal, &replaced);
        pVal->Release();
    }
}

static void ApplyColorMetadata(void* pEncodingProps, BOOL isOutputStream) {
    if (!pEncodingProps || !g_OptimizeColor) return;

    HMODULE hCombase = GetModuleHandleW(L"combase.dll");
    if (!hCombase) hCombase = LoadLibraryW(L"combase.dll");
    if (!hCombase) return;

    typedef HRESULT (WINAPI *CreateStrRefFn)(LPCWSTR, UINT32, HSTRING_HEADER_LOCAL*, void**);
    typedef HRESULT (WINAPI *RoGetActFactFn)(void*, REFIID, void**);

    CreateStrRefFn pCreateStrRef = (CreateStrRefFn)(void*)GetProcAddress(hCombase, "WindowsCreateStringReference");
    RoGetActFactFn pRoGetFact    = (RoGetActFactFn)(void*)GetProcAddress(hCombase, "RoGetActivationFactory");
    if (!pCreateStrRef || !pRoGetFact) return;

    static const WCHAR kPropValClass[] = L"Windows.Foundation.PropertyValue";
    HSTRING_HEADER_LOCAL hdr = {};
    void* hstr = NULL;
    if (FAILED(pCreateStrRef(kPropValClass, (UINT32)(sizeof(kPropValClass) / sizeof(WCHAR) - 1), &hdr, &hstr))) {
        return;
    }

    IUnknown* pStatics = NULL;
    if (FAILED(pRoGetFact(hstr, IID_IPropertyValueStatics, (void**)&pStatics)) || !pStatics) {
        return;
    }

    IUnknown* pMediaProps = NULL;
    if (SUCCEEDED(((IUnknown*)pEncodingProps)->QueryInterface(IID_IMediaEncodingProperties, (void**)&pMediaProps)) && pMediaProps) {
        typedef HRESULT (__stdcall *GetPropsFn)(void*, void**);
        GetPropsFn getProps = (GetPropsFn)(*(void***)pMediaProps)[6];
        void* pMap = NULL;
        if (SUCCEEDED(getProps(pMediaProps, &pMap)) && pMap) {
            // Standard BT.709 color gamut & transfer matching modern displays/browsers
            SetMediaPropUInt32(pMap, pStatics, kMF_MT_VIDEO_PRIMARIES,   2); // MFVideoPrimaries_BT709
            SetMediaPropUInt32(pMap, pStatics, kMF_MT_TRANSFER_FUNCTION, 5); // MFVideoTransFunc_709

            if (isOutputStream) {
                // YUV output explicitly tagged BT.709 Limited Range (16-235) for universal player fidelity
                SetMediaPropUInt32(pMap, pStatics, kMF_MT_YUV_MATRIX,          1); // MFVideoTransferMatrix_BT709
                SetMediaPropUInt32(pMap, pStatics, kMF_MT_VIDEO_NOMINAL_RANGE, 2); // MFNominalRange_16_235
            } else {
                // Uncompressed BGRA8 desktop capture is Full Range RGB (0-255)
                SetMediaPropUInt32(pMap, pStatics, kMF_MT_VIDEO_NOMINAL_RANGE, 1); // MFNominalRange_0_255
            }

            ((IUnknown*)pMap)->Release();
        }
        pMediaProps->Release();
    }

    pStatics->Release();
}

extern "C" HRESULT __stdcall ConfigureVideoEncodingHook(void* pVideoProps, UINT32 bitrate) {
    if (!pVideoProps) return E_POINTER;

    LoadConfig(FALSE);

    typedef HRESULT (__stdcall *PutBitrateFn)(void*, UINT32);
    PutBitrateFn putBitrate = (PutBitrateFn)(*(void***)pVideoProps)[6];
    HRESULT hr = putBitrate(pVideoProps, bitrate);

    if (g_OptimizeColor) {
        // Upgrade H.264 Main (77) -> High (100) for sharper text/fine UI detail
        IUnknown* pProps2 = NULL;
        HRESULT hrProf = E_FAIL;
        if (SUCCEEDED(((IUnknown*)pVideoProps)->QueryInterface(IID_IVideoEncodingProperties2, (void**)&pProps2)) && pProps2) {
            typedef HRESULT (__stdcall *PutProfileIdFn)(void*, INT32);
            PutProfileIdFn putProfileId = (PutProfileIdFn)(*(void***)pProps2)[8];
            hrProf = putProfileId(pProps2, 100);
            pProps2->Release();
        }

        ApplyColorMetadata(pVideoProps, TRUE);
        Log("[HighSnippingFrames] Encoder profile tuned: Bitrate=%u ProfileHigh->0x%08X Color=BT709",
            bitrate, hrProf);
    }

    return hr;
}

extern "C" HRESULT __stdcall CreateVideoStreamDescriptorHook(void* pFactory, void* pUncompressedVideoProps, void** ppDescOut) {
    if (!pFactory) return E_POINTER;

    if (pUncompressedVideoProps && g_OptimizeColor) {
        ApplyColorMetadata(pUncompressedVideoProps, FALSE);
    }

    typedef HRESULT (__stdcall *CreateDescFn)(void*, void*, void**);
    CreateDescFn origCreate = (CreateDescFn)(*(void***)pFactory)[6];
    return origCreate(pFactory, pUncompressedVideoProps, ppDescOut);
}

static void UnlockCaptureSession(void* pSession) {
    if (!pSession || !g_UnlockCompositor) return;

    IUnknown* pUnk = (IUnknown*)pSession;
    void* pSession5 = NULL;
    if (SUCCEEDED(pUnk->QueryInterface(IID_IGraphicsCaptureSession5, &pSession5)) && pSession5) {
        typedef HRESULT (__stdcall *PutIntervalFn)(void*, INT64);
        PutIntervalFn putInterval = (PutIntervalFn)(*(void***)pSession5)[7];

        // 100ns interval with 15% VSync jitter headroom
        INT64 interval100ns = (g_TargetFPS > 0) ? ((10000000LL * 85) / ((INT64)g_TargetFPS * 100)) : 0;
        HRESULT hrPut = putInterval(pSession5, interval100ns);
        Log("[HighSnippingFrames] MinUpdateInterval=%lld (%.2f ms) for %u FPS -> 0x%08X",
            interval100ns, (double)interval100ns / 10000.0, g_TargetFPS, hrPut);

        ((IUnknown*)pSession5)->Release();
    }
}

extern "C" HRESULT __stdcall StartCaptureSessionHook(void* pSession) {
    if (!pSession) return E_POINTER;

    LoadConfig(FALSE);
    if (g_ConfiguredFPS <= 0 && g_PerMonitorMatch) {
        UINT32 liveHz = DetectActiveMonitorRefreshRate();
        if (liveHz != g_TargetFPS) {
            g_TargetFPS = liveHz;
            if (g_pPatch2Imm)     PatchImm32(g_pPatch2Imm, ComputeMinIntervalMs(g_TargetFPS));
            if (g_pQueueLimitImm) PatchImm32(g_pQueueLimitImm, ComputeQueueLimitFrames(g_TargetFPS));
        }
    }

    UnlockCaptureSession(pSession);
    PublishStatus();

    typedef HRESULT (__stdcall *StartCaptureFn)(void*);
    StartCaptureFn origStart = (StartCaptureFn)(*(void***)pSession)[6];
    return origStart(pSession);
}

extern "C" __declspec(dllexport) UINT32 MediaRatioHook(void** ppRatio) {
    LoadConfig(FALSE);

    void* pRetAddr = GET_RETURN_ADDRESS();
    BOOL isPacingCaller = (g_pMatch2 &&
                           (BYTE*)pRetAddr >= (g_pMatch2 - 64) &&
                           (BYTE*)pRetAddr <= (g_pMatch2 + 64));

    if (!ppRatio || !*ppRatio) {
        return isPacingCaller ? g_TargetFPS : GetBitrateBasis(g_TargetFPS, g_BitrateMode);
    }

    void* pRatio = *ppRatio;
    void** vtbl = *(void***)pRatio;
    if (vtbl) {
        typedef HRESULT (__stdcall *PutRatioFn)(void*, UINT32);
        PutRatioFn putNumerator   = (PutRatioFn)vtbl[6];
        PutRatioFn putDenominator = (PutRatioFn)vtbl[8];
        UINT32 num = (g_TargetFPS == 59) ? 60 : g_TargetFPS;
        putNumerator(pRatio, num);
        putDenominator(pRatio, 1);
    }

    return isPacingCaller ? g_TargetFPS : GetBitrateBasis(g_TargetFPS, g_BitrateMode);
}

static BYTE* FindPattern(BYTE* pBase, SIZE_T size, const BYTE* pPat, SIZE_T patLen) {
    if (!pBase || !pPat || patLen == 0 || size < patLen) return NULL;
    for (SIZE_T i = 0; i <= size - patLen; i++) {
        if (pBase[i] == pPat[0] && memcmp(&pBase[i], pPat, patLen) == 0) {
            return &pBase[i];
        }
    }
    return NULL;
}

static BYTE* FindPatternMask(BYTE* pBase, SIZE_T size, const BYTE* pPat, const char* mask) {
    SIZE_T patLen = strlen(mask);
    if (!pBase || !pPat || patLen == 0 || size < patLen) return NULL;
    for (SIZE_T i = 0; i <= size - patLen; i++) {
        BOOL match = TRUE;
        for (SIZE_T j = 0; j < patLen; j++) {
            if (mask[j] == 'x' && pBase[i + j] != pPat[j]) {
                match = FALSE;
                break;
            }
        }
        if (match) return &pBase[i];
    }
    return NULL;
}

static void ApplySnippingToolPatches() {
    if (InterlockedCompareExchange(&g_Patched, 1, 0) != 0) return;

    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    if (!base) return;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    BYTE* textBase = NULL;
    SIZE_T textSize = 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            textBase = base + sec->VirtualAddress;
            textSize = sec->Misc.VirtualSize;
            break;
        }
    }
    if (!textBase || textSize < 64) {
        Log("[HighSnippingFrames] .text section unavailable");
        return;
    }

    UINT32 mask = 0;

    // Patch 1: OnFrameArrived 10ms micro-jitter frame dropper (7d 0e -> eb 0e)
    if (g_BypassThrottle) {
        const BYTE p1[] = { 0x10, 0x27, 0x00, 0x00, 0x48, 0x3B, 0xC1, 0x7D, 0x0E };
        BYTE* m1 = FindPattern(textBase, textSize, p1, sizeof(p1));
        if (m1) {
            BYTE* pJge = m1 + 7;
            DWORD oldProt = 0;
            if (VirtualProtect(pJge, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
                pJge[0] = 0xEB;
                VirtualProtect(pJge, 2, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pJge, 2);
                mask |= 0x01;
            }
        }
    }

    // Patch 2: VideoFrameGenerator::Initialize 1000/fps math -> calibrated interval ms
    if (g_UnlockEncoder) {
        const BYTE p2[] = { 0x33, 0xD2, 0xB8, 0xE8, 0x03, 0x00, 0x00, 0xF7, 0xF1 };
        BYTE* m2 = FindPattern(textBase, textSize, p2, sizeof(p2));
        if (m2) {
            g_pMatch2 = m2;
            BYTE* pMath = m2 + 2;
            UINT32 minMs = ComputeMinIntervalMs(g_TargetFPS);
            DWORD oldProt = 0;
            if (VirtualProtect(pMath, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
                pMath[0] = 0xB8;
                memcpy(&pMath[1], &minMs, sizeof(UINT32));
                pMath[5] = 0x90;
                pMath[6] = 0x90;
                g_pPatch2Imm = &pMath[1];
                VirtualProtect(pMath, 7, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), pMath, 7);
                mask |= 0x02;
            }
        }
    }

    // Patch 3: IMediaRatio query -> MediaRatioHook via RtlLookupFunctionEntry
    if (g_UnlockEncoder) {
        const BYTE p3[] = { 0xC7, 0x44, 0x24, 0x20, 0xAF, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x38 };
        BYTE* m3 = FindPattern(textBase, textSize, p3, sizeof(p3));
        if (m3) {
            DWORD64 imgBase = 0;
            PRUNTIME_FUNCTION pRt = RtlLookupFunctionEntry((DWORD64)m3, &imgBase, NULL);
            BYTE* fnStart = pRt ? ((BYTE*)imgBase + pRt->BeginAddress) : (m3 - 0x2F);
            DWORD oldProt = 0;
            if (VirtualProtect(fnStart, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR target = (ULONG_PTR)MediaRatioHook;
                BYTE jmp14[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
                memcpy(&jmp14[6], &target, sizeof(ULONG_PTR));
                memcpy(fnStart, jmp14, 14);
                VirtualProtect(fnStart, 14, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), fnStart, 14);
                mask |= 0x04;
            }
        }
    }

    // Patch 4: VideoFrameGenerator::StartCapture -> StartCaptureSessionHook (13-byte call rax, CFG-safe)
    if (g_UnlockCompositor) {
        const BYTE pQpc[]  = { 0xE8, 0x03, 0x00, 0x00, 0x48, 0x99, 0x48, 0xF7 };
        const BYTE pCall[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x30, 0xFF, 0x15 };
        BYTE* mQpc = FindPattern(textBase, textSize, pQpc, sizeof(pQpc));
        BYTE* callSite = NULL;
        if (mQpc) {
            SIZE_T avail = (SIZE_T)((textBase + textSize) - mQpc);
            callSite = FindPattern(mQpc, avail < 120 ? avail : 120, pCall, sizeof(pCall));
        }
        if (callSite) {
            DWORD oldProt = 0;
            if (VirtualProtect(callSite, 13, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR target = (ULONG_PTR)StartCaptureSessionHook;
                BYTE patch13[13] = { 0x48, 0xB8 };
                memcpy(&patch13[2], &target, sizeof(ULONG_PTR));
                patch13[10] = 0xFF;
                patch13[11] = 0xD0;
                patch13[12] = 0x90;
                memcpy(callSite, patch13, 13);
                VirtualProtect(callSite, 13, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), callSite, 13);
                mask |= 0x08;
            }
        }
    }

    // Patch 5: Scale OnFrameArrived queue headroom & remove 80/100 mid-queue 50%/90% frame slicer
    if (g_PreventDrops) {
        const BYTE pQueue[] = { 0x48, 0x81, 0xFF, 0x96, 0x00, 0x00, 0x00, 0x0F, 0x82 };
        BYTE* mQueue = FindPattern(textBase, textSize, pQueue, sizeof(pQueue));
        if (mQueue) {
            g_pQueueLimitImm = mQueue + 3;
            PatchImm32(g_pQueueLimitImm, ComputeQueueLimitFrames(g_TargetFPS));
            mask |= 0x10;

            SIZE_T avail = (SIZE_T)((textBase + textSize) - mQueue);
            SIZE_T win = avail < 800 ? avail : 800;
            const BYTE pCmp100[] = { 0x48, 0x83, 0xFF, 0x64, 0x72 };
            const BYTE pCmp80[]  = { 0x48, 0x83, 0xFF, 0x50, 0x72 };
            BYTE* m100 = FindPattern(mQueue + 9, win > 9 ? win - 9 : 0, pCmp100, sizeof(pCmp100));
            if (m100) {
                BYTE* m80 = FindPattern(m100 + 5, 64, pCmp80, sizeof(pCmp80));
                if (m80) {
                    BYTE* enqueueTarget = m80 + 6 + (int8_t)m80[5];
                    ptrdiff_t disp = enqueueTarget - (m100 + 2);
                    if (disp >= 6 && disp < 127) {
                        DWORD oldProt = 0;
                        if (VirtualProtect(m100, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
                            m100[0] = 0xEB;
                            m100[1] = (BYTE)disp;
                            m100[2] = 0x90;
                            m100[3] = 0x90;
                            m100[4] = 0x90;
                            m100[5] = 0x90;
                            VirtualProtect(m100, 6, oldProt, &oldProt);
                            FlushInstructionCache(GetCurrentProcess(), m100, 6);
                            mask |= 0x20;
                        }
                    }
                }
            }
        }

        // Patch 6: Double Direct3D11CaptureFramePool buffer depth (10 -> 20 frames) for burst smoothness
        const BYTE pPool[] = { 0xC7, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x15 };
        BYTE* mPool = FindPatternMask(textBase, textSize, pPool, "xxx??xxxxxxxxx");
        if (mPool) {
            BYTE* pBufImm = mPool + 7;
            PatchImm32(pBufImm, 20u);
            mask |= 0x40;
        }
    }

    // Patch 7: Upgrade H.264 High Profile + BT.709 color tags (Output Profile & Input Stream Descriptor)
    if (g_OptimizeColor) {
        const BYTE pPutBitrate[] = { 0x48, 0x8B, 0x03, 0x41, 0x8B, 0xD6, 0x48, 0x8B, 0xCB, 0x48, 0x8B, 0x40, 0x30, 0xFF, 0x15 };
        BYTE* mBitrate = FindPattern(textBase, textSize, pPutBitrate, sizeof(pPutBitrate));
        if (mBitrate) {
            DWORD oldProt = 0;
            if (VirtualProtect(mBitrate, 19, PAGE_EXECUTE_READWRITE, &oldProt)) {
                ULONG_PTR target = (ULONG_PTR)ConfigureVideoEncodingHook;
                BYTE patch19[19] = { 0x48, 0x89, 0xD9, 0x44, 0x89, 0xF2, 0x48, 0xB8 };
                memcpy(&patch19[8], &target, sizeof(ULONG_PTR));
                patch19[16] = 0xFF;
                patch19[17] = 0xD0;
                patch19[18] = 0x90;
                memcpy(mBitrate, patch19, 19);
                VirtualProtect(mBitrate, 19, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), mBitrate, 19);
                mask |= 0x80;
            }
        }

        const BYTE pCreateDesc[] = {
            0x48, 0x8B, 0x01,
            0x4C, 0x8D, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x94, 0x24, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x8B, 0x40, 0x30,
            0xFF, 0x15
        };
        BYTE* mDesc = NULL;
        if (mBitrate) {
            BYTE* searchStart = mBitrate + 0x200;
            if (searchStart < textBase + textSize) {
                SIZE_T rem = (SIZE_T)((textBase + textSize) - searchStart);
                mDesc = FindPatternMask(searchStart, rem < 0x280 ? rem : 0x280,
                                        pCreateDesc, "xxxxxxx????xxxx????xxxxxx");
            }
        }
        if (mDesc) {
            DWORD oldProt = 0;
            if (VirtualProtect(mDesc, 29, PAGE_EXECUTE_READWRITE, &oldProt)) {
                BYTE patch29[29];
                // Preserve exact lea r8, [rsp+disp32] and mov rdx, [rsp+disp32] instructions
                memcpy(&patch29[0],  &mDesc[3],  8);
                memcpy(&patch29[8],  &mDesc[11], 8);
                ULONG_PTR target = (ULONG_PTR)CreateVideoStreamDescriptorHook;
                patch29[16] = 0x48;
                patch29[17] = 0xB8;
                memcpy(&patch29[18], &target, sizeof(ULONG_PTR));
                patch29[26] = 0xFF;
                patch29[27] = 0xD0;
                patch29[28] = 0x90;
                memcpy(mDesc, patch29, 29);
                VirtualProtect(mDesc, 29, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), mDesc, 29);
                mask |= 0x100;
            }
        }
    }

    g_PatchMask = mask;
    PublishStatus();
    Log("[HighSnippingFrames] Attached PID=%lu TargetFPS=%u BitrateMode=%d Mask=0x%X",
        GetCurrentProcessId(), g_TargetFPS, g_BitrateMode, mask);
    if ((mask & 0xF) != 0xF) {
        Log("[HighSnippingFrames] Warning: core timing mask=0x%X (p1=%d p2=%d p3=%d p4=%d)",
            mask & 0xF, (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = hModule;
        DisableThreadLibraryCalls(hModule);

        // Pin module to avoid accidental unload while hooks are active
        HMODULE pinned = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)hModule, &pinned);

        EnsureLock();
        LoadConfig(TRUE);
        ApplySnippingToolPatches();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (InterlockedCompareExchange(&g_csReady, 2, 2) == 2) {
            DeleteCriticalSection(&g_csLock);
        }
    }
    return TRUE;
}
