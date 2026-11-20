#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <dwmapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>
#include <stdint.h>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

// Resource & Message IDs
#define IDI_APPICON             101
#define WM_TRAYICON             (WM_APP + 1)
#define WM_APP_SHOW_UI          (WM_APP + 2)
#define TIMER_BACKUP_SCAN       1
#define HOTKEY_RECORD           1

// Tray Menu IDs
#define IDM_SHOW_UI             1001
#define IDM_FPS_AUTO            1002
#define IDM_FPS_60              1003
#define IDM_FPS_90              1004
#define IDM_FPS_120             1005
#define IDM_FPS_144             1006
#define IDM_FPS_165             1007
#define IDM_FPS_240             1008
#define IDM_LAUNCH_SNIP         1009
#define IDM_OPEN_RECORDINGS     1010
#define IDM_RESCAN              1011
#define IDM_EXIT                1012

// Clean Dark Palette
static const COLORREF CLR_BG         = RGB(17, 20, 26);
static const COLORREF CLR_CARD       = RGB(24, 28, 36);
static const COLORREF CLR_CARD_BORDER= RGB(42, 48, 60);
static const COLORREF CLR_BTN        = RGB(33, 39, 50);
static const COLORREF CLR_BTN_HOVER  = RGB(45, 53, 68);
static const COLORREF CLR_ACCENT     = RGB(59, 130, 246);
static const COLORREF CLR_ACCENT_HOV = RGB(79, 147, 255);
static const COLORREF CLR_GREEN      = RGB(16, 185, 129);
static const COLORREF CLR_GREEN_BG   = RGB(17, 49, 41);
static const COLORREF CLR_BLUE_BG    = RGB(24, 42, 71);
static const COLORREF CLR_AMBER      = RGB(245, 158, 11);
static const COLORREF CLR_AMBER_BG   = RGB(54, 38, 17);
static const COLORREF CLR_TEXT       = RGB(243, 244, 246);
static const COLORREF CLR_MUTED      = RGB(156, 163, 175);
static const COLORREF CLR_CLOSE_HOV  = RGB(185, 42, 52);

// Logical window size (scaled by DPI)
static const int BASE_W = 444;
static const int BASE_H = 452;

// Application state
static HINSTANCE        g_hInst             = NULL;
static HWND             g_hWnd              = NULL;
static NOTIFYICONDATAW  g_nid               = {};
static HWINEVENTHOOK    g_hWinEventShow     = NULL;
static HWINEVENTHOOK    g_hWinEventFg       = NULL;
static UINT             g_dpi               = 96;
static UINT             g_wmTaskbarCreated  = 0;
static WCHAR            g_lastTooltip[128]  = L"";

// Paths
static WCHAR            g_exeDir[MAX_PATH]  = L"";
static WCHAR            g_dllPath[MAX_PATH] = L"";
static WCHAR            g_iniPath[MAX_PATH] = L"";
static WCHAR            g_statusPath[MAX_PATH] = L"";
static BOOL             g_isPortable        = FALSE;
static BOOL             g_aclChecked        = FALSE;

// Settings
static int              g_targetFPS         = 0;     // 0 = Auto
static int              g_bitrateMode       = 0;     // 0=Balanced, 1=High, 2=Ultra
static BOOL             g_autoStart         = FALSE;
static BOOL             g_hotkeyEnabled     = TRUE;
static BOOL             g_perMonitorMatch   = TRUE;
static BOOL             g_unlockCompositor  = TRUE;
static BOOL             g_unlockEncoder     = TRUE;
static BOOL             g_bypassThrottle    = TRUE;
static BOOL             g_preventDrops      = TRUE;
static BOOL             g_optimizeColor     = TRUE;
static UINT32           g_detectedHz        = 60;

// Live SnippingTool attachment tracking (no 200ms polling!)
struct AttachedProc {
    DWORD  pid;
    HANDLE hSync;
};
static const int        MAX_ATTACHED        = 16;
static AttachedProc     g_attached[MAX_ATTACHED] = {};
static int              g_attachedCount     = 0;

static const int        MAX_SEEN_PIDS       = 2048;
static DWORD            g_knownPids[MAX_SEEN_PIDS] = {};
static int              g_knownPidCount     = 0;

static UINT32           g_liveMask          = 0;
static int              g_liveCount         = 0;
static UINT32           g_liveActiveFPS     = 0;

// Interactive hit rectangles (logical UI elements in physical pixels)
enum ControlId {
    CTL_NONE = 0,
    CTL_MIN,
    CTL_CLOSE,
    CTL_FPS_0,
    CTL_FPS_1,
    CTL_FPS_2,
    CTL_FPS_3,
    CTL_FPS_4,
    CTL_FPS_5,
    CTL_FPS_6,
    CTL_BR_0,
    CTL_BR_1,
    CTL_BR_2,
    CTL_CHK_STARTUP,
    CTL_CHK_HOTKEY,
    CTL_BTN_LAUNCH,
    CTL_BTN_RECORDINGS,
    CTL_BTN_RESCAN,
    CTL_COUNT
};

static const int        FPS_VALUES[7]       = { 0, 60, 90, 120, 144, 165, 240 };
static const WCHAR*     FPS_LABELS[7]       = { L"AUTO", L"60", L"90", L"120", L"144", L"165", L"240" };
static const WCHAR*     BR_LABELS[3]        = { L"Balanced", L"High", L"Ultra" };

static RECT             g_ctlRects[CTL_COUNT] = {};
static ControlId        g_hoverCtl          = CTL_NONE;
static BOOL             g_trackingMouse     = FALSE;

// Cached GDI fonts (only recreated on DPI change or show)
static HFONT            g_fontTitle         = NULL;
static HFONT            g_fontBadge         = NULL;
static HFONT            g_fontSection       = NULL;
static HFONT            g_fontBody          = NULL;
static HFONT            g_fontBold          = NULL;
static HFONT            g_fontSmall         = NULL;

// Temporary backbuffer freed whenever window hides
static HBITMAP          g_backBmp           = NULL;
static int              g_backW             = 0;
static int              g_backH             = 0;

// Forward declarations
static void CheckPidForSnippingTool(DWORD pid);
static void ScanProcessesOnce(BOOL checkAll);
static void UpdateTrayTooltip();
static void RefreshStatusFromFile();

static int S(int px) {
    return MulDiv(px, (int)g_dpi, 96);
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

static void DestroyFonts() {
    HFONT* fonts[] = { &g_fontTitle, &g_fontBadge, &g_fontSection, &g_fontBody, &g_fontBold, &g_fontSmall };
    for (int i = 0; i < 6; i++) {
        if (*fonts[i]) {
            DeleteObject(*fonts[i]);
            *fonts[i] = NULL;
        }
    }
}

static void EnsureFonts() {
    if (g_fontTitle) return;
    auto mkFont = [](int pt, int weight) -> HFONT {
        return CreateFontW(-S(pt), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    g_fontTitle   = mkFont(15, FW_SEMIBOLD);
    g_fontBadge   = mkFont(10, FW_SEMIBOLD);
    g_fontSection = mkFont(10, FW_SEMIBOLD);
    g_fontBody    = mkFont(13, FW_NORMAL);
    g_fontBold    = mkFont(13, FW_SEMIBOLD);
    g_fontSmall   = mkFont(11, FW_NORMAL);
}

static void FreeBackbuffer() {
    if (g_backBmp) {
        DeleteObject(g_backBmp);
        g_backBmp = NULL;
    }
    g_backW = 0;
    g_backH = 0;
}

static void InitPaths() {
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    WCHAR* sl = wcsrchr(g_exeDir, L'\\');
    if (sl) *(sl + 1) = L'\0';

    _snwprintf(g_dllPath, MAX_PATH, L"%lsSnippingToolHighFPS.dll", g_exeDir);

    // Detect whether running from Program Files or a portable directory
    WCHAR pfDir[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"ProgramFiles", pfDir, MAX_PATH);
    g_isPortable = !(pfDir[0] && _wcsnicmp(g_exeDir, pfDir, wcslen(pfDir)) == 0);

    WCHAR progDataDir[MAX_PATH] = L"";
    WCHAR progData[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH)) {
        _snwprintf(progDataDir, MAX_PATH, L"%ls\\SnippingToolHighFPS", progData);
        CreateDirectoryW(progDataDir, NULL);
    }

    WCHAR localIni[MAX_PATH] = L"";
    _snwprintf(localIni, MAX_PATH, L"%lsSnippingToolHighFPS.ini", g_exeDir);

    // Check whether local INI is writable
    BOOL localWritable = FALSE;
    HANDLE hTest = CreateFileW(localIni, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTest != INVALID_HANDLE_VALUE) {
        CloseHandle(hTest);
        localWritable = TRUE;
    }

    if (g_isPortable && localWritable) {
        wcsncpy(g_iniPath, localIni, MAX_PATH - 1);
    } else if (progDataDir[0]) {
        _snwprintf(g_iniPath, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", progDataDir);
        if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(localIni) != INVALID_FILE_ATTRIBUTES) {
            CopyFileW(localIni, g_iniPath, TRUE);
        }
    } else {
        wcsncpy(g_iniPath, localIni, MAX_PATH - 1);
    }

    if (progDataDir[0]) {
        _snwprintf(g_statusPath, MAX_PATH, L"%ls\\patch_status.ini", progDataDir);
    } else {
        _snwprintf(g_statusPath, MAX_PATH, L"%lspatch_status.ini", g_exeDir);
    }
}

// Ensures Store AppContainer (S-1-15-2-1) can load our DLL when running in Portable Mode
static void EnsureAppContainerReadableOnce() {
    if (g_aclChecked) return;
    g_aclChecked = TRUE;
    if (!g_isPortable) return; // Program Files install already has ACL configured by install.ps1

    PSID pAppSid = NULL;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &pAppSid)) return;

    PSECURITY_DESCRIPTOR pSD = NULL;
    PACL pOldDacl = NULL;
    if (GetNamedSecurityInfoW(g_exeDir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &pOldDacl, NULL, &pSD) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea = {};
        ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
        ea.grfAccessMode        = GRANT_ACCESS;
        ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName    = (LPWSTR)pAppSid;

        PACL pNewDacl = NULL;
        if (SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl) == ERROR_SUCCESS && pNewDacl) {
            SetNamedSecurityInfoW(g_exeDir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDacl, NULL);
            LocalFree(pNewDacl);
        }
        LocalFree(pSD);
    }
    LocalFree(pAppSid);
}

static BOOL QueryStartupEnabled() {
    HKEY hKey = NULL;
    BOOL enabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR val[MAX_PATH * 2] = L"";
        DWORD cb = sizeof(val);
        if (RegQueryValueExW(hKey, L"HighSnippingFrames", NULL, NULL, (BYTE*)val, &cb) == ERROR_SUCCESS) {
            enabled = (val[0] != L'\0');
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

static void SetStartupEnabled(BOOL enable) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        WCHAR exePath[MAX_PATH] = L"";
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        WCHAR cmd[MAX_PATH + 32] = L"";
        _snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --minimized", exePath);
        DWORD cb = (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR));
        RegSetValueExW(hKey, L"HighSnippingFrames", 0, REG_SZ, (const BYTE*)cmd, cb);
    } else {
        RegDeleteValueW(hKey, L"HighSnippingFrames");
    }
    RegCloseKey(hKey);
    g_autoStart = enable;
}

static void LoadSettings() {
    g_detectedHz = DetectPrimaryRefreshRate();
    if (g_iniPath[0]) {
        g_targetFPS        = (int)GetPrivateProfileIntW(L"Settings", L"TargetFPS", 0, g_iniPath);
        g_bitrateMode      = (int)GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 0, g_iniPath);
        g_perMonitorMatch  = GetPrivateProfileIntW(L"Settings", L"PerMonitorAutoMatch", 1, g_iniPath) != 0;
        g_hotkeyEnabled    = GetPrivateProfileIntW(L"Settings", L"EnableGlobalHotkey", 1, g_iniPath) != 0;
        g_unlockCompositor = GetPrivateProfileIntW(L"Settings", L"UnlockCompositor", 1, g_iniPath) != 0;
        g_unlockEncoder    = GetPrivateProfileIntW(L"Settings", L"UnlockEncoder", 1, g_iniPath) != 0;
        g_bypassThrottle   = GetPrivateProfileIntW(L"Settings", L"BypassThrottle", 1, g_iniPath) != 0;
        g_preventDrops     = GetPrivateProfileIntW(L"Settings", L"PreventStartupDrops", 1, g_iniPath) != 0;
        g_optimizeColor    = GetPrivateProfileIntW(L"Settings", L"OptimizeColor", 1, g_iniPath) != 0;
    }
    if (g_bitrateMode < 0) g_bitrateMode = 0;
    if (g_bitrateMode > 2) g_bitrateMode = 2;
    g_autoStart = QueryStartupEnabled();
}

static void SaveSettings() {
    if (!g_iniPath[0]) return;
    WCHAR buf[32];
    _snwprintf(buf, 32, L"%d", g_targetFPS);
    WritePrivateProfileStringW(L"Settings", L"TargetFPS", buf, g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"PerMonitorAutoMatch", g_perMonitorMatch ? L"1" : L"0", g_iniPath);

    _snwprintf(buf, 32, L"%d", g_bitrateMode);
    WritePrivateProfileStringW(L"Settings", L"BitrateMultiplier", buf, g_iniPath);

    WritePrivateProfileStringW(L"Settings", L"EnableGlobalHotkey", g_hotkeyEnabled ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"UnlockCompositor",   g_unlockCompositor ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"UnlockEncoder",      g_unlockEncoder ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"BypassThrottle",     g_bypassThrottle ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"PreventStartupDrops",g_preventDrops ? L"1" : L"0", g_iniPath);
    WritePrivateProfileStringW(L"Settings", L"OptimizeColor",      g_optimizeColor ? L"1" : L"0", g_iniPath);
}

static void ApplyHotkeyState() {
    if (!g_hWnd) return;
    UnregisterHotKey(g_hWnd, HOTKEY_RECORD);
    if (g_hotkeyEnabled) {
        RegisterHotKey(g_hWnd, HOTKEY_RECORD, MOD_CONTROL | MOD_ALT, 'R');
    }
}

static void GetRecordingsFolder(WCHAR* outPath, DWORD maxLen) {
    // FOLDERID_Videos {18989B1D-99B5-455B-841C-AB7C74E4DDFC}
    static const KNOWNFOLDERID kVideosId = {
        0x18989B1D, 0x99B5, 0x455B, { 0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC }
    };
    outPath[0] = L'\0';
    PWSTR videosPath = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(kVideosId, 0, NULL, &videosPath)) && videosPath) {
        _snwprintf(outPath, maxLen, L"%ls\\Screen Recordings", videosPath);
        CoTaskMemFree(videosPath);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return;

        // Fall back to Videos if Screen Recordings folder has not been created yet
        PWSTR v2 = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(kVideosId, 0, NULL, &v2)) && v2) {
            wcsncpy(outPath, v2, maxLen - 1);
            outPath[maxLen - 1] = L'\0';
            CoTaskMemFree(v2);
        }
    }
}

static BOOL IsProcessAttached(DWORD pid) {
    for (int i = 0; i < g_attachedCount; i++) {
        if (g_attached[i].pid == pid) return TRUE;
    }
    return FALSE;
}

static void PruneExitedAttached() {
    int writeIdx = 0;
    for (int i = 0; i < g_attachedCount; i++) {
        if (g_attached[i].hSync && WaitForSingleObject(g_attached[i].hSync, 0) == WAIT_TIMEOUT) {
            g_attached[writeIdx++] = g_attached[i];
        } else if (g_attached[i].hSync) {
            CloseHandle(g_attached[i].hSync);
        }
    }
    g_attachedCount = writeIdx;
}

static BOOL IsModuleLoadedInProcess(HANDLE hProc, const WCHAR* targetBaseName) {
    HMODULE mods[512];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(hProc, mods, sizeof(mods), &cbNeeded)) {
        return FALSE;
    }
    DWORD count = cbNeeded / sizeof(HMODULE);
    if (count > 512) count = 512;
    for (DWORD i = 0; i < count; i++) {
        WCHAR modName[MAX_PATH] = L"";
        if (GetModuleBaseNameW(hProc, mods[i], modName, MAX_PATH)) {
            if (_wcsicmp(modName, targetBaseName) == 0) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void AttachToSnippingTool(DWORD pid) {
    if (IsProcessAttached(pid)) return;
    if (GetFileAttributesW(g_dllPath) == INVALID_FILE_ATTRIBUTES) return;

    EnsureAppContainerReadableOnce();

    // Least-privilege access needed for module check + LoadLibraryW call
    DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;
    HANDLE hProc = OpenProcess(access, FALSE, pid);
    if (!hProc) return;

    if (!IsModuleLoadedInProcess(hProc, L"SnippingToolHighFPS.dll")) {
        SIZE_T pathBytes = (wcslen(g_dllPath) + 1) * sizeof(WCHAR);
        void* remoteMem = VirtualAllocEx(hProc, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remoteMem) {
            if (WriteProcessMemory(hProc, remoteMem, g_dllPath, pathBytes, NULL)) {
                HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
                FARPROC pLoadLib = hK32 ? GetProcAddress(hK32, "LoadLibraryW") : NULL;
                if (pLoadLib) {
                    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
                                                        (LPTHREAD_START_ROUTINE)(void*)pLoadLib,
                                                        remoteMem, 0, NULL);
                    if (hThread) {
                        WaitForSingleObject(hThread, 2500);
                        CloseHandle(hThread);
                    }
                }
            }
            VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        }
    }

    // Track lifetime via lightweight SYNCHRONIZE handle
    if (g_attachedCount < MAX_ATTACHED) {
        HANDLE hSync = NULL;
        if (DuplicateHandle(GetCurrentProcess(), hProc, GetCurrentProcess(), &hSync, SYNCHRONIZE, FALSE, 0)) {
            g_attached[g_attachedCount++] = { pid, hSync };
        }
    }
    CloseHandle(hProc);

    RefreshStatusFromFile();
    UpdateTrayTooltip();
    if (g_hWnd && IsWindowVisible(g_hWnd)) {
        InvalidateRect(g_hWnd, NULL, FALSE);
    }
}

static BOOL IsKnownPid(DWORD pid) {
    for (int i = 0; i < g_knownPidCount; i++) {
        if (g_knownPids[i] == pid) return TRUE;
    }
    return FALSE;
}

static void CheckPidForSnippingTool(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId() || IsProcessAttached(pid)) return;

    HANDLE hQuery = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hQuery) return;

    WCHAR exePath[MAX_PATH] = L"";
    DWORD size = MAX_PATH;
    BOOL match = FALSE;
    if (QueryFullProcessImageNameW(hQuery, 0, exePath, &size)) {
        const WCHAR* base = wcsrchr(exePath, L'\\');
        base = base ? (base + 1) : exePath;
        if (_wcsicmp(base, L"SnippingTool.exe") == 0) {
            match = TRUE;
        }
    }
    CloseHandle(hQuery);

    if (match) {
        AttachToSnippingTool(pid);
    }
}

// Sparse PID-diff backup scan (only inspects brand-new PIDs, catching headless SnippingTool.exe)
static void ScanProcessesOnce(BOOL checkAll) {
    int prevAttached = g_attachedCount;
    UINT32 prevMask  = g_liveMask;
    UINT32 prevFps   = g_liveActiveFPS;
    PruneExitedAttached();

    DWORD pids[MAX_SEEN_PIDS];
    DWORD cbNeeded = 0;
    if (!EnumProcesses(pids, sizeof(pids), &cbNeeded)) return;

    int count = (int)(cbNeeded / sizeof(DWORD));
    if (count > MAX_SEEN_PIDS) count = MAX_SEEN_PIDS;

    for (int i = 0; i < count; i++) {
        DWORD pid = pids[i];
        if (pid == 0) continue;
        if (checkAll || !IsKnownPid(pid)) {
            CheckPidForSnippingTool(pid);
        }
    }

    // Refresh known PID snapshot (immune to Windows PID reuse after process exit)
    g_knownPidCount = count;
    for (int i = 0; i < count; i++) {
        g_knownPids[i] = pids[i];
    }

    RefreshStatusFromFile();
    if (prevAttached != g_attachedCount || prevMask != g_liveMask || prevFps != g_liveActiveFPS) {
        UpdateTrayTooltip();
        if (g_hWnd && IsWindowVisible(g_hWnd)) {
            InvalidateRect(g_hWnd, NULL, FALSE);
        }
    }
}

static void RefreshStatusFromFile() {
    if (!g_statusPath[0] || g_attachedCount == 0) {
        g_liveMask = 0;
        g_liveCount = 0;
        g_liveActiveFPS = 0;
        return;
    }

    UINT32 filePid = GetPrivateProfileIntW(L"Status", L"PID", 0, g_statusPath);
    BOOL pidAlive = FALSE;
    for (int i = 0; i < g_attachedCount; i++) {
        if (g_attached[i].pid == filePid) {
            pidAlive = TRUE;
            break;
        }
    }
    if (!pidAlive) {
        g_liveMask = 0;
        g_liveCount = 0;
        g_liveActiveFPS = 0;
        return;
    }

    WCHAR maskStr[32] = L"0";
    GetPrivateProfileStringW(L"Status", L"PatchMask", L"0", maskStr, 32, g_statusPath);
    g_liveMask      = (UINT32)wcstoul(maskStr, NULL, 0);
    g_liveCount     = (int)GetPrivateProfileIntW(L"Status", L"PatchCount", 0, g_statusPath);
    g_liveActiveFPS = GetPrivateProfileIntW(L"Status", L"ActiveFPS", 0, g_statusPath);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !hwnd) return;
    if (hwnd != GetAncestor(hwnd, GA_ROOT)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || IsKnownPid(pid) || IsProcessAttached(pid)) return;

    if (g_knownPidCount < MAX_SEEN_PIDS) {
        g_knownPids[g_knownPidCount++] = pid;
    }
    CheckPidForSnippingTool(pid);
}

static int EffectiveFPS() {
    return (g_targetFPS > 0) ? g_targetFPS : (int)g_detectedHz;
}

static void UpdateTrayTooltip() {
    if (!g_hWnd) return;

    WCHAR tip[128];
    const WCHAR* state = (g_attachedCount > 0) ? ((g_liveCount == 4) ? L"Active" : L"Attached") : L"Ready";
    _snwprintf(tip, 128, L"High Snipping Frames - %d FPS (%ls)", EffectiveFPS(), state);

    if (wcscmp(tip, g_lastTooltip) == 0) return;
    wcsncpy(g_lastTooltip, tip, 127);

    g_nid.uFlags = NIF_TIP;
    wcsncpy(g_nid.szTip, tip, 127);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void LaunchSnippingTool() {
    ShellExecuteW(NULL, L"open", L"ms-screenclip:", NULL, NULL, SW_SHOWNORMAL);
}

static void OpenRecordingsFolder() {
    WCHAR folder[MAX_PATH] = L"";
    GetRecordingsFolder(folder, MAX_PATH);
    if (folder[0]) {
        ShellExecuteW(NULL, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void ShowControlCenter() {
    if (!g_hWnd) return;
    LoadSettings();
    RefreshStatusFromFile();
    UpdateTrayTooltip();
    ShowWindow(g_hWnd, SW_SHOW);
    SetForegroundWindow(g_hWnd);
    InvalidateRect(g_hWnd, NULL, FALSE);
}

static void HideControlCenter() {
    if (!g_hWnd) return;
    ShowWindow(g_hWnd, SW_HIDE);
    FreeBackbuffer();
}

// Pure Win32 GDI drawing helpers
static void FillRoundRect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH hBr = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, hBr);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

static void DrawTextRect(HDC hdc, const WCHAR* text, RECT r, HFONT font, COLORREF clr, UINT flags) {
    HGDIOBJ oldF = SelectObject(hdc, font);
    SetTextColor(hdc, clr);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldF);
}

static void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdcScreen = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    EnsureFonts();

    if (!g_backBmp || g_backW != w || g_backH != h) {
        FreeBackbuffer();
        g_backBmp = CreateCompatibleBitmap(hdcScreen, w, h);
        g_backW = w;
        g_backH = h;
    }

    HDC hdc = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldBmp = SelectObject(hdc, g_backBmp);

    // Clear background + crisp 1px outer border
    HBRUSH hBg = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    HPEN hOuter = CreatePen(PS_SOLID, 1, CLR_CARD_BORDER);
    HGDIOBJ oldP = SelectObject(hdc, hOuter);
    HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, w, h);
    SelectObject(hdc, oldP);
    SelectObject(hdc, oldB);
    DeleteObject(hOuter);

    int pad = S(18);
    int rad = S(10);
    int btnRad = S(7);

    // 1. Top Header Bar
    HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, S(20), S(20), 0);
    if (hIcon) {
        DrawIconEx(hdc, pad, S(16), hIcon, S(20), S(20), 0, NULL, DI_NORMAL);
        DestroyIcon(hIcon);
    }

    RECT rTitle = { pad + S(28), S(12), w - S(140), S(40) };
    DrawTextRect(hdc, L"High Snipping Frames", rTitle, g_fontTitle, CLR_TEXT, DT_LEFT | DT_VCENTER);

    RECT rVer = { w - S(148), S(16), w - S(96), S(36) };
    FillRoundRect(hdc, rVer, S(6), CLR_BLUE_BG, CLR_BLUE_BG);
    DrawTextRect(hdc, L"v2.0", rVer, g_fontBadge, CLR_ACCENT_HOV, DT_CENTER | DT_VCENTER);

    // Minimize & Close caption buttons
    g_ctlRects[CTL_MIN]   = { w - S(82), S(12), w - S(50), S(38) };
    g_ctlRects[CTL_CLOSE] = { w - S(44), S(12), w - S(12), S(38) };
    FillRoundRect(hdc, g_ctlRects[CTL_MIN], btnRad,
                  g_hoverCtl == CTL_MIN ? CLR_BTN_HOVER : CLR_BG, CLR_BG);
    DrawTextRect(hdc, L"\u2013", g_ctlRects[CTL_MIN], g_fontBold, CLR_MUTED, DT_CENTER | DT_VCENTER);

    FillRoundRect(hdc, g_ctlRects[CTL_CLOSE], btnRad,
                  g_hoverCtl == CTL_CLOSE ? CLR_CLOSE_HOV : CLR_BG, CLR_BG);
    DrawTextRect(hdc, L"\u00D7", g_ctlRects[CTL_CLOSE], g_fontBold,
                 g_hoverCtl == CTL_CLOSE ? CLR_TEXT : CLR_MUTED, DT_CENTER | DT_VCENTER);

    // 2. Status Card
    int y = S(56);
    RECT rStatusCard = { pad, y, w - pad, y + S(78) };
    FillRoundRect(hdc, rStatusCard, rad, CLR_CARD, CLR_CARD_BORDER);

    BOOL isActive = (g_attachedCount > 0);
    BOOL fullMask = (isActive && g_liveCount == 4);
    COLORREF pillBg = isActive ? (fullMask ? CLR_GREEN_BG : CLR_AMBER_BG) : CLR_BLUE_BG;
    COLORREF pillFg = isActive ? (fullMask ? CLR_GREEN : CLR_AMBER) : CLR_ACCENT_HOV;
    const WCHAR* pillTxt = isActive ? (fullMask ? L"\u25CF  Active (4/4 Hooks)" : L"\u25CF  Attached")
                                    : L"\u25CF  Ready \u2022 Auto-Attach";

    RECT rPill = { pad + S(14), y + S(12), pad + S(186), y + S(36) };
    FillRoundRect(hdc, rPill, S(12), pillBg, pillBg);
    DrawTextRect(hdc, pillTxt, rPill, g_fontBadge, pillFg, DT_CENTER | DT_VCENTER);

    WCHAR modeTag[48];
    _snwprintf(modeTag, 48, L"%ls Mode", g_isPortable ? L"Portable" : L"Installed");
    RECT rMode = { w - pad - S(130), y + S(12), w - pad - S(14), y + S(36) };
    DrawTextRect(hdc, modeTag, rMode, g_fontSmall, CLR_MUTED, DT_RIGHT | DT_VCENTER);

    WCHAR subLine[128];
    int eff = EffectiveFPS();
    _snwprintf(subLine, 128, L"Display: %u Hz   \u2022   Target: %d FPS   \u2022   Bitrate: %ls",
               g_detectedHz, eff, BR_LABELS[g_bitrateMode]);
    RECT rSub = { pad + S(14), y + S(44), w - pad - S(14), y + S(66) };
    DrawTextRect(hdc, subLine, rSub, g_fontBody, CLR_MUTED, DT_LEFT | DT_VCENTER);

    // 3. Target Frame Rate Card
    y += S(90);
    RECT rFpsCard = { pad, y, w - pad, y + S(96) };
    FillRoundRect(hdc, rFpsCard, rad, CLR_CARD, CLR_CARD_BORDER);

    RECT rFpsLbl = { pad + S(14), y + S(12), w - pad - S(140), y + S(32) };
    DrawTextRect(hdc, L"TARGET FRAME RATE", rFpsLbl, g_fontSection, CLR_MUTED, DT_LEFT | DT_VCENTER);

    WCHAR effHint[48];
    _snwprintf(effHint, 48, g_targetFPS == 0 ? L"Matched: %u FPS" : L"Locked: %d FPS", eff);
    RECT rFpsHint = { w - pad - S(150), y + S(12), w - pad - S(14), y + S(32) };
    DrawTextRect(hdc, effHint, rFpsHint, g_fontSmall, CLR_ACCENT_HOV, DT_RIGHT | DT_VCENTER);

    int innerW = (w - 2 * pad) - S(28);
    int gap = S(6);
    int btnW = (innerW - gap * 6) / 7;
    int btnX = pad + S(14);
    int btnY = y + S(42);
    for (int i = 0; i < 7; i++) {
        ControlId cid = (ControlId)(CTL_FPS_0 + i);
        RECT rb = { btnX + i * (btnW + gap), btnY, btnX + i * (btnW + gap) + btnW, btnY + S(38) };
        if (i == 6) rb.right = w - pad - S(14);
        g_ctlRects[cid] = rb;

        BOOL sel = (g_targetFPS == FPS_VALUES[i]);
        BOOL hov = (g_hoverCtl == cid);
        COLORREF fill = sel ? CLR_ACCENT : (hov ? CLR_BTN_HOVER : CLR_BTN);
        COLORREF brd  = sel ? CLR_ACCENT : CLR_CARD_BORDER;
        FillRoundRect(hdc, rb, btnRad, fill, brd);
        DrawTextRect(hdc, FPS_LABELS[i], rb, g_fontBold, CLR_TEXT, DT_CENTER | DT_VCENTER);
    }

    // 4. Bitrate Profile Card
    y += S(108);
    RECT rBrCard = { pad, y, w - pad, y + S(92) };
    FillRoundRect(hdc, rBrCard, rad, CLR_CARD, CLR_CARD_BORDER);

    RECT rBrLbl = { pad + S(14), y + S(12), w - pad - S(14), y + S(30) };
    DrawTextRect(hdc, L"ENCODING BITRATE PROFILE", rBrLbl, g_fontSection, CLR_MUTED, DT_LEFT | DT_VCENTER);

    int brBtnW = (innerW - gap * 2) / 3;
    int brY = y + S(38);
    const WCHAR* brSub[3] = { L"Compact ~20 Mbps", L"Crisp ~27 Mbps", L"Ultra ~35 Mbps" };
    for (int i = 0; i < 3; i++) {
        ControlId cid = (ControlId)(CTL_BR_0 + i);
        RECT rb = { btnX + i * (brBtnW + gap), brY, btnX + i * (brBtnW + gap) + brBtnW, brY + S(40) };
        if (i == 2) rb.right = w - pad - S(14);
        g_ctlRects[cid] = rb;

        BOOL sel = (g_bitrateMode == i);
        BOOL hov = (g_hoverCtl == cid);
        COLORREF fill = sel ? CLR_ACCENT : (hov ? CLR_BTN_HOVER : CLR_BTN);
        FillRoundRect(hdc, rb, btnRad, fill, sel ? CLR_ACCENT : CLR_CARD_BORDER);

        RECT topT = { rb.left, rb.top + S(4), rb.right, rb.top + S(21) };
        RECT botT = { rb.left, rb.top + S(20), rb.right, rb.bottom - S(3) };
        DrawTextRect(hdc, BR_LABELS[i], topT, g_fontBold, CLR_TEXT, DT_CENTER | DT_VCENTER);
        DrawTextRect(hdc, brSub[i], botT, g_fontSmall, sel ? CLR_TEXT : CLR_MUTED, DT_CENTER | DT_VCENTER);
    }

    // 5. Options & Actions
    y += S(104);
    g_ctlRects[CTL_CHK_STARTUP] = { pad, y, w / 2 - S(4), y + S(28) };
    g_ctlRects[CTL_CHK_HOTKEY]  = { w / 2 + S(4), y, w - pad, y + S(28) };

    auto drawCheckbox = [&](const RECT& r, BOOL checked, BOOL hovered, const WCHAR* label) {
        int boxSz = S(16);
        RECT box = { r.left + S(2), r.top + (r.bottom - r.top - boxSz) / 2,
                     r.left + S(2) + boxSz, r.top + (r.bottom - r.top + boxSz) / 2 };
        FillRoundRect(hdc, box, S(4),
                      checked ? CLR_ACCENT : (hovered ? CLR_BTN_HOVER : CLR_BTN),
                      checked ? CLR_ACCENT : CLR_CARD_BORDER);
        if (checked) {
            DrawTextRect(hdc, L"\u2713", box, g_fontBadge, CLR_TEXT, DT_CENTER | DT_VCENTER);
        }
        RECT txtR = { box.right + S(8), r.top, r.right, r.bottom };
        DrawTextRect(hdc, label, txtR, g_fontBody, hovered ? CLR_TEXT : CLR_MUTED, DT_LEFT | DT_VCENTER);
    };

    drawCheckbox(g_ctlRects[CTL_CHK_STARTUP], g_autoStart,     g_hoverCtl == CTL_CHK_STARTUP, L"Start with Windows");
    drawCheckbox(g_ctlRects[CTL_CHK_HOTKEY],  g_hotkeyEnabled, g_hoverCtl == CTL_CHK_HOTKEY,  L"Ctrl+Alt+R hotkey");

    // Primary & Secondary Bottom Buttons
    y += S(38);
    int actGap = S(10);
    int halfW = (w - 2 * pad - actGap) / 2;
    g_ctlRects[CTL_BTN_LAUNCH]     = { pad, y, pad + halfW - S(34), y + S(38) };
    g_ctlRects[CTL_BTN_RECORDINGS] = { pad + halfW - S(34) + actGap, y, w - pad - S(68) - actGap, y + S(38) };
    g_ctlRects[CTL_BTN_RESCAN]     = { w - pad - S(68), y, w - pad, y + S(38) };

    FillRoundRect(hdc, g_ctlRects[CTL_BTN_LAUNCH], btnRad,
                  g_hoverCtl == CTL_BTN_LAUNCH ? CLR_ACCENT_HOV : CLR_ACCENT, CLR_ACCENT);
    DrawTextRect(hdc, L"Launch Snipping Tool", g_ctlRects[CTL_BTN_LAUNCH], g_fontBold, CLR_TEXT, DT_CENTER | DT_VCENTER);

    FillRoundRect(hdc, g_ctlRects[CTL_BTN_RECORDINGS], btnRad,
                  g_hoverCtl == CTL_BTN_RECORDINGS ? CLR_BTN_HOVER : CLR_BTN, CLR_CARD_BORDER);
    DrawTextRect(hdc, L"Recordings", g_ctlRects[CTL_BTN_RECORDINGS], g_fontBody, CLR_TEXT, DT_CENTER | DT_VCENTER);

    FillRoundRect(hdc, g_ctlRects[CTL_BTN_RESCAN], btnRad,
                  g_hoverCtl == CTL_BTN_RESCAN ? CLR_BTN_HOVER : CLR_BTN, CLR_CARD_BORDER);
    DrawTextRect(hdc, L"Rescan", g_ctlRects[CTL_BTN_RESCAN], g_fontSmall, CLR_MUTED, DT_CENTER | DT_VCENTER);

    BitBlt(hdcScreen, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static ControlId HitTestCtl(POINT pt) {
    for (int i = 1; i < CTL_COUNT; i++) {
        if (PtInRect(&g_ctlRects[i], pt)) return (ControlId)i;
    }
    return CTL_NONE;
}

static void HandleClick(ControlId cid) {
    if (cid == CTL_MIN || cid == CTL_CLOSE) {
        HideControlCenter();
        return;
    }
    if (cid >= CTL_FPS_0 && cid <= CTL_FPS_6) {
        g_targetFPS = FPS_VALUES[cid - CTL_FPS_0];
        SaveSettings();
        UpdateTrayTooltip();
        InvalidateRect(g_hWnd, NULL, FALSE);
        return;
    }
    if (cid >= CTL_BR_0 && cid <= CTL_BR_2) {
        g_bitrateMode = (int)(cid - CTL_BR_0);
        SaveSettings();
        InvalidateRect(g_hWnd, NULL, FALSE);
        return;
    }
    if (cid == CTL_CHK_STARTUP) {
        SetStartupEnabled(!g_autoStart);
        InvalidateRect(g_hWnd, NULL, FALSE);
        return;
    }
    if (cid == CTL_CHK_HOTKEY) {
        g_hotkeyEnabled = !g_hotkeyEnabled;
        SaveSettings();
        ApplyHotkeyState();
        InvalidateRect(g_hWnd, NULL, FALSE);
        return;
    }
    if (cid == CTL_BTN_LAUNCH) {
        LaunchSnippingTool();
        return;
    }
    if (cid == CTL_BTN_RECORDINGS) {
        OpenRecordingsFolder();
        return;
    }
    if (cid == CTL_BTN_RESCAN) {
        ScanProcessesOnce(TRUE);
        InvalidateRect(g_hWnd, NULL, FALSE);
    }
}

static void ShowTrayMenu(HWND hwnd) {
    LoadSettings();
    RefreshStatusFromFile();

    HMENU hMenu = CreatePopupMenu();
    HMENU hFpsSub = CreatePopupMenu();

    for (int i = 0; i < 7; i++) {
        WCHAR item[48];
        if (i == 0) _snwprintf(item, 48, L"Auto (%u Hz)", g_detectedHz);
        else        _snwprintf(item, 48, L"%d FPS", FPS_VALUES[i]);

        UINT flags = MF_STRING | ((g_targetFPS == FPS_VALUES[i]) ? MF_CHECKED : 0);
        AppendMenuW(hFpsSub, flags, IDM_FPS_AUTO + i, item);
    }

    AppendMenuW(hMenu, MF_STRING, IDM_SHOW_UI, L"Open Control Center");
    SetMenuDefaultItem(hMenu, IDM_SHOW_UI, FALSE);
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFpsSub, L"Target Framerate");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_LAUNCH_SNIP, L"Launch Snipping Tool");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_RECORDINGS, L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_RESCAN, L"Rescan Processes");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_dpi = GetDpiForWindow(hwnd);
            if (g_dpi == 0) g_dpi = 96;
            DWORD pref = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
            SetTimer(hwnd, TIMER_BACKUP_SCAN, 3000, NULL);
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            DestroyFonts();
            FreeBackbuffer();
            RECT* prc = (RECT*)lParam;
            SetWindowPos(hwnd, NULL, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            if (pt.y < S(48) && !PtInRect(&g_ctlRects[CTL_MIN], pt) && !PtInRect(&g_ctlRects[CTL_CLOSE], pt)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            if (!g_trackingMouse) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_trackingMouse = TRUE;
            }
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ControlId hit = HitTestCtl(pt);
            if (hit != g_hoverCtl) {
                g_hoverCtl = hit;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_trackingMouse = FALSE;
            if (g_hoverCtl != CTL_NONE) {
                g_hoverCtl = CTL_NONE;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            HandleClick(HitTestCtl(pt));
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT && g_hoverCtl != CTL_NONE) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                HideControlCenter();
                return 0;
            }
            return 0;

        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_TIMER:
            if (wParam == TIMER_BACKUP_SCAN) {
                ScanProcessesOnce(FALSE);
            }
            return 0;

        case WM_HOTKEY:
            if (wParam == HOTKEY_RECORD) {
                LaunchSnippingTool();
            }
            return 0;

        case WM_DISPLAYCHANGE:
            g_detectedHz = DetectPrimaryRefreshRate();
            UpdateTrayTooltip();
            if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_APP_SHOW_UI:
            ShowControlCenter();
            return 0;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                if (IsWindowVisible(hwnd)) HideControlCenter();
                else ShowControlCenter();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COMMAND: {
            UINT id = LOWORD(wParam);
            if (id == IDM_SHOW_UI) {
                ShowControlCenter();
            } else if (id >= IDM_FPS_AUTO && id <= IDM_FPS_240) {
                g_targetFPS = FPS_VALUES[id - IDM_FPS_AUTO];
                SaveSettings();
                UpdateTrayTooltip();
                if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
            } else if (id == IDM_LAUNCH_SNIP) {
                LaunchSnippingTool();
            } else if (id == IDM_OPEN_RECORDINGS) {
                OpenRecordingsFolder();
            } else if (id == IDM_RESCAN) {
                ScanProcessesOnce(TRUE);
                if (IsWindowVisible(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
            } else if (id == IDM_EXIT) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_CLOSE:
            HideControlCenter();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_BACKUP_SCAN);
            UnregisterHotKey(hwnd, HOTKEY_RECORD);
            if (g_hWinEventShow) UnhookWinEvent(g_hWinEventShow);
            if (g_hWinEventFg)   UnhookWinEvent(g_hWinEventFg);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            for (int i = 0; i < g_attachedCount; i++) {
                if (g_attached[i].hSync) CloseHandle(g_attached[i].hSync);
            }
            FreeBackbuffer();
            DestroyFonts();
            PostQuitMessage(0);
            return 0;
    }
    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated) {
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_hInst = hInstance;

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HighSnippingFramesGUI_v2_SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"HighSnippingFrames_v2_Wnd", NULL);
        if (hExisting) PostMessageW(hExisting, WM_APP_SHOW_UI, 0, 0);
        CloseHandle(hMutex);
        return 0;
    }

    InitPaths();
    LoadSettings();
    if (g_iniPath[0] && GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        SaveSettings(); // Create default INI only on first run
    }
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"HighSnippingFrames_v2_Wnd";
    RegisterClassExW(&wc);

    HDC hdcScreen = GetDC(NULL);
    g_dpi = (UINT)GetDeviceCaps(hdcScreen, LOGPIXELSX);
    ReleaseDC(NULL, hdcScreen);
    if (g_dpi == 0) g_dpi = 96;

    int winW = S(BASE_W);
    int winH = S(BASE_H);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    g_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"High Snipping Frames",
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
        (screenW - winW) / 2, (screenH - winH) / 2, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!g_hWnd) {
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // Tray icon setup (zero unsolicited balloons)
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wcsncpy(g_nid.szTip, L"High Snipping Frames", 127);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Event-driven window hooks + initial baseline scan
    g_hWinEventShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL,
                                      WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hWinEventFg   = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                                      WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    ScanProcessesOnce(TRUE);
    UpdateTrayTooltip();
    ApplyHotkeyState();

    BOOL startMinimized = (lpCmdLine &&
                           (wcsstr(lpCmdLine, L"--minimized") ||
                            wcsstr(lpCmdLine, L"-minimized") ||
                            wcsstr(lpCmdLine, L"--tray")));
    if (!startMinimized) {
        ShowControlCenter();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    return (int)msg.wParam;
}
