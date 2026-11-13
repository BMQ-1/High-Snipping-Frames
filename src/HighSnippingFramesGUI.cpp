#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

#define WM_TRAYICON (WM_USER + 101)
#define ID_TRAY_APP_ICON 1001

// Menu IDs
#define IDM_OPEN          3001
#define IDM_FPS_AUTO      3010
#define IDM_FPS_60        3011
#define IDM_FPS_90        3012
#define IDM_FPS_120       3013
#define IDM_FPS_144       3014
#define IDM_FPS_165       3015
#define IDM_FPS_240       3016
#define IDM_RECORD        3020
#define IDM_FOLDER        3021
#define IDM_ANALYZE       3022
#define IDM_RESTART_SVC   3023
#define IDM_RESET_ZOOM    3024
#define IDM_STORAGE_CHECK 3026
#define IDM_REPAIR        3027
#define IDM_STORAGE_AUTO  3028
#define IDM_EXIT          3025

#define HOTKEY_RECORD_VIDEO 2001

// Internal Timers
#define TIMER_SLEEP_RECOVERY    4001
#define TIMER_STORAGE_GUARDIAN  4002
#define TIMER_STORAGE_STARTUP   4003
#define TIMER_STATUS_REFRESH    4004

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// Base logical design resolution (100% zoom)
const int BASE_WIDTH  = 520;
const int BASE_HEIGHT = 580;

// Global application state
static HINSTANCE g_hInstance = NULL;
static HWND g_hWndMain = NULL;
static NOTIFYICONDATAW g_nid;
static ULONG_PTR g_gdiplusToken = 0;
static WCHAR g_IniPath[MAX_PATH] = { 0 };
static HANDLE g_hStopEvent = NULL;

static UINT32 g_CurrentFPS = 144;
static int    g_QualityLevel = 0; // 0=Balanced (Default, compact), 1=High, 2=Ultra
static float  g_ZoomFactor = 1.0f; // Zoom: 0.8f - 2.0f
static BOOL   g_AutoStart = TRUE;
static DWORD  g_LastSavedTick = 0; // For temporary "✓ Saved" toast animation
static BOOL   g_LastSavedSuccess = TRUE;
static BOOL   g_bPortableMode = FALSE;
static BOOL   g_LastInjectError = FALSE;
static DWORD  g_LastUiSnipPid = 0;
static int    g_LastUiAppliedPatches = -1;

// Storage Guardian Configuration
static BOOL  g_EnableStorageGuardian   = TRUE;
static DWORD g_MaxFolderSizeGB         = 15;        // Alert if folder exceeds size in GB (0 = disabled)
static DWORD g_TransferDays            = 0;         // Transfer recordings older than X days to Folder X (0 = disabled)
static WCHAR g_TransferFolder[MAX_PATH] = { 0 };    // Destination Folder X
static DWORD g_AutoCleanDays           = 0;         // Auto-delete recordings older than X days (0 = disabled)

// Global Hotkey & Clipboard Fast-Share
static BOOL  g_EnableGlobalHotkey      = TRUE;
static BOOL  g_AutoCopyToClipboard     = TRUE;
static BOOL  g_bHotkeyRegistered       = FALSE;
static WCHAR g_szActiveHotkey[32]      = L"Win + Alt + R";
static UINT  g_uMsgTaskbarCreated      = 0;

// Hit test IDs (in base logical coordinates)
#define HIT_NONE           0
#define HIT_FPS_PRESET     1
#define HIT_QUALITY_PRESET 2
#define HIT_AUTOSTART      3
#define HIT_BTN_RECORD     4
#define HIT_BTN_FOLDER     5
#define HIT_BTN_ANALYZE    6
#define HIT_ZOOM_MINUS     7
#define HIT_ZOOM_PLUS      8
#define HIT_ZOOM_RESET     9
#define HIT_ALERT_REPAIR   10

static int g_HoveredHitId = HIT_NONE;
static int g_HoveredData = -1;

// FPS presets: 0 = Auto, followed by fixed rates
static const UINT32 g_FpsOptions[] = { 0, 60, 90, 120, 144, 165, 240 };
static const WCHAR* g_FpsLabels[]  = { L"AUTO", L"60", L"90", L"120", L"144", L"165", L"240" };
static const int g_NumFpsOptions   = 7;

// Quality presets (optimized for shareable bitrates and Discord/Slack compatibility)
static const WCHAR* g_QualityLabels[] = { L"Balanced (1.0x)", L"High Quality (1.5x)", L"Ultra Gaming (2.0x)" };
static const WCHAR* g_QualityDescs[]  = { L"Crisp & Shareable (16-30 Mbps)", L"Enhanced Detail (22-45 Mbps)", L"Maximum Fidelity (30-65 Mbps)" };
static const int g_NumQualityOptions = 3;

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

// Grants ALL APPLICATION PACKAGES (S-1-15-2-1) read (+ optional execute) permission
// so SnippingTool.exe can load SnippingToolHighFPS.dll cleanly in Portable Mode on any PC
static void EnsureAppContainerAccess(const WCHAR* filePath, DWORD accessMask) {
    if (!filePath || GetFileAttributesW(filePath) == INVALID_FILE_ATTRIBUTES) return;

    PSID pAppPkgSid = NULL;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &pAppPkgSid)) return;

    PACL pOldDacl = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetNamedSecurityInfoW(filePath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &pOldDacl, NULL, &pSD) == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea = { 0 };
        ea.grfAccessPermissions = accessMask;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)pAppPkgSid;

        PACL pNewDacl = NULL;
        if (SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl) == ERROR_SUCCESS && pNewDacl) {
            SetNamedSecurityInfoW((LPWSTR)filePath, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDacl, NULL);
            LocalFree(pNewDacl);
        }
        if (pSD) LocalFree(pSD);
    }
    LocalFree(pAppPkgSid);
}

// Helper: Get active Snipping Tool Screen Recordings folder (supporting localized/OneDrive Videos and fallback Captures)
// FOLDERID_Captures: {EDC86909-BE97-4FEB-AC0F-7F02E698AC47}
static const GUID GUID_FOLDERID_Captures = 
    { 0xedc86909, 0xbe97, 0x4feb, { 0xac, 0x0f, 0x7f, 0x02, 0xe6, 0x98, 0xac, 0x47 } };

// FOLDERID_Videos: {18989B1D-99A5-45da-BD91-5216B61E79B6}
static const GUID GUID_FOLDERID_Videos = 
    { 0x18989b1d, 0x99a5, 0x45da, { 0xbd, 0x91, 0x52, 0x16, 0xb6, 0x1e, 0x79, 0xb6 } };

static BOOL GetRecordingsFolder(WCHAR* outPath, DWORD maxLen) {
    // 1. Snipping Tool's default Windows 11 target: FOLDERID_Videos \ Screen Recordings (covers OneDrive KFM & redirected Libraries)
    PWSTR pKnownVideos = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(GUID_FOLDERID_Videos, 0, NULL, &pKnownVideos)) && pKnownVideos) {
        swprintf(outPath, maxLen, L"%ls\\Screen Recordings", pKnownVideos);
        CoTaskMemFree(pKnownVideos);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
        // Create Videos\Screen Recordings so the directory watcher tracks new recordings immediately
        if (CreateDirectoryW(outPath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            return TRUE;
        }
    }

    // 2. Fallback to USERPROFILE \ Videos \ Screen Recordings
    WCHAR userProfile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH)) {
        swprintf(outPath, maxLen, L"%ls\\Videos\\Screen Recordings", userProfile);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES ||
            CreateDirectoryW(outPath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            return TRUE;
        }
    }

    // 3. Final fallback to FOLDERID_Captures (Xbox Game Bar / older custom setups)
    PWSTR pKnownCaptures = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(GUID_FOLDERID_Captures, 0, NULL, &pKnownCaptures)) && pKnownCaptures) {
        if (GetFileAttributesW(pKnownCaptures) != INVALID_FILE_ATTRIBUTES) {
            wcsncpy(outPath, pKnownCaptures, maxLen - 1);
            outPath[maxLen - 1] = L'\0';
            CoTaskMemFree(pKnownCaptures);
            return TRUE;
        }
        CoTaskMemFree(pKnownCaptures);
    }

    return FALSE;
}

static void UpdateTrayTooltip();

static BOOL IsFileWritableOrCreatable(const WCHAR* path) {
    if (!path || !path[0]) return FALSE;
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        HANDLE hTest = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);
            return TRUE;
        }
        return FALSE;
    }
    HANDLE hCreate = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hCreate != INVALID_HANDLE_VALUE) {
        CloseHandle(hCreate);
        DeleteFileW(path);
        return TRUE;
    }
    return FALSE;
}

// Locate writable INI path with clean separation between Portable Mode and Installed Mode
static void LocateIniPath() {
    WCHAR selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *pSlash = L'\0';

    // 1. Portable Mode: executable is outside Program Files and directory INI is writable
    if (!IsUnderProgramFiles(selfDir)) {
        WCHAR portableIni[MAX_PATH];
        swprintf(portableIni, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", selfDir);
        if (IsFileWritableOrCreatable(portableIni)) {
            wcscpy(g_IniPath, portableIni);
            g_bPortableMode = TRUE;
            return;
        }
    }

    // 2. Installed Mode: %PROGRAMDATA%\SnippingToolHighFPS\SnippingToolHighFPS.ini (writable for non-admin Users, readable by AppContainer)
    WCHAR progData[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH)) {
        WCHAR pdDir[MAX_PATH];
        swprintf(pdDir, MAX_PATH, L"%ls\\SnippingToolHighFPS", progData);
        CreateDirectoryW(pdDir, NULL);

        WCHAR pdIni[MAX_PATH];
        swprintf(pdIni, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", pdDir);
        if (IsFileWritableOrCreatable(pdIni)) {
            wcscpy(g_IniPath, pdIni);
            g_bPortableMode = FALSE;
            return;
        }
    }

    // 3. Per-user AppData fallback if ProgramData is restricted
    WCHAR localApp[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH)) {
        WCHAR laDir[MAX_PATH];
        swprintf(laDir, MAX_PATH, L"%ls\\HighSnippingFrames", localApp);
        CreateDirectoryW(laDir, NULL);
        swprintf(g_IniPath, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", laDir);
        return;
    }

    swprintf(g_IniPath, MAX_PATH, L"%ls\\SnippingToolHighFPS.ini", selfDir);
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

// Helper: Detect highest active refresh rate
static UINT32 GetMaxDisplayRefresh() {
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

// Helper: Detect if a conflicting legacy IFEO hook is registered for SnippingTool.exe
static BOOL IsLegacyIFEOActive() {
    HKEY hKey;
    LSTATUS st = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SnippingTool.exe",
        0, KEY_READ, &hKey);
    if (st == ERROR_SUCCESS) {
        WCHAR dbg[MAX_PATH] = { 0 };
        DWORD len = sizeof(dbg);
        LSTATUS valSt = RegQueryValueExW(hKey, L"Debugger", NULL, NULL, (LPBYTE)dbg, &len);
        RegCloseKey(hKey);
        return (valSt == ERROR_SUCCESS && len > 0);
    }
    return FALSE;
}

// Helper: Remove legacy IFEO Debugger hook
static void RepairSnippingToolAssociations(HWND hWnd) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SnippingTool.exe",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, L"Debugger");
        RegCloseKey(hKey);
    }

    if (!IsLegacyIFEOActive()) {
        if (hWnd) {
            MessageBoxW(hWnd,
                L"Windows Snipping Tool shortcuts have been successfully verified!\n\n"
                L"Win + Shift + S and Print Screen will work cleanly and instantly.",
                L"High Snipping Frames - Repair", MB_ICONINFORMATION);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return;
    }

    LPCWSTR script = L"-ExecutionPolicy Bypass -NoProfile -Command "
                     L"\"Remove-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SnippingTool.exe' -Name 'Debugger' -ErrorAction SilentlyContinue; "
                     L"Remove-ItemProperty -Path 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\ScreenClippingHost.exe' -Name 'Debugger' -ErrorAction SilentlyContinue\"";

    HINSTANCE hInst = ShellExecuteW(NULL, L"runas", L"powershell.exe", script, NULL, SW_HIDE);
    if ((INT_PTR)hInst > 32) {
        Sleep(1200);
        if (!IsLegacyIFEOActive()) {
            if (hWnd) {
                MessageBoxW(hWnd,
                    L"Windows Snipping Tool shortcuts have been successfully repaired!",
                    L"High Snipping Frames - Repair", MB_ICONINFORMATION);
            }
        }
    }
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}

// Find installed SnippingTool.exe path
static BOOL FindInstalledSnippingToolPath(WCHAR* outPath, DWORD maxLen) {
    WCHAR searchPath[MAX_PATH] = L"C:\\Program Files\\WindowsApps\\Microsoft.ScreenSketch_*_x64__8wekyb3d8bbwe";
    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
        swprintf(outPath, maxLen, L"C:\\Program Files\\WindowsApps\\%ls\\SnippingTool\\SnippingTool.exe", wfd.cFileName);
        FindClose(hFind);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
    }

    WCHAR localApp[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH)) {
        swprintf(outPath, maxLen, L"%ls\\Microsoft\\WindowsApps\\SnippingTool.exe", localApp);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
    }
    return FALSE;
}

// Helper: Check if Process is running
static DWORD FindProcessPID(const WCHAR* name) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

// Helper: Check AutoStart in HKCU Run
static BOOL IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(hKey, L"HighSnippingFrames", NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (status == ERROR_SUCCESS);
    }
    return FALSE;
}

// Helper: Set AutoStart in HKCU Run (Single Unified Entry)
static void SetAutoStart(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR selfExe[MAX_PATH];
            GetModuleFileNameW(NULL, selfExe, MAX_PATH);
            WCHAR guiPath[MAX_PATH + 32];
            swprintf(guiPath, MAX_PATH + 32, L"\"%ls\" --minimized", selfExe);
            RegSetValueExW(hKey, L"HighSnippingFrames", 0, REG_SZ, (const BYTE*)guiPath, (DWORD)(wcslen(guiPath) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"HighSnippingFrames");
        }
        RegDeleteValueW(hKey, L"SnippingToolHighFPSDaemon");
        RegDeleteValueW(hKey, L"HighSnippingFramesGUI");
        RegCloseKey(hKey);
    }
}

// -------------------------------------------------------------
// Integrated Background Injection Watcher (Unified Engine)
// -------------------------------------------------------------
static volatile BOOL g_bWatcherRunning = TRUE;
static HANDLE g_hWatcherThread = NULL;

static void LocateDllPath(WCHAR* outPath) {
    WCHAR selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *pSlash = L'\0';

    swprintf(outPath, MAX_PATH, L"%ls\\SnippingToolHighFPS.dll", selfDir);
    if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(outPath, L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolHighFPS.dll");
    }
}

// Copies file path to Windows Clipboard using CF_HDROP format (instant paste in Discord/Slack)
static BOOL CopyFileToClipboard(const WCHAR* filePath) {
    if (!filePath || GetFileAttributesW(filePath) == INVALID_FILE_ATTRIBUTES) return FALSE;

    if (!OpenClipboard(NULL)) return FALSE;
    EmptyClipboard();

    SIZE_T len = wcslen(filePath);
    SIZE_T dropFilesSize = sizeof(DROPFILES) + (len + 2) * sizeof(WCHAR);

    HGLOBAL hGlobal = GlobalAlloc(GHND, dropFilesSize);
    if (!hGlobal) {
        CloseClipboard();
        return FALSE;
    }

    DROPFILES* pDropFiles = (DROPFILES*)GlobalLock(hGlobal);
    if (!pDropFiles) {
        GlobalFree(hGlobal);
        CloseClipboard();
        return FALSE;
    }

    pDropFiles->pFiles = sizeof(DROPFILES);
    pDropFiles->pt.x = 0;
    pDropFiles->pt.y = 0;
    pDropFiles->fNC = FALSE;
    pDropFiles->fWide = TRUE;

    WCHAR* pData = (WCHAR*)((BYTE*)pDropFiles + sizeof(DROPFILES));
    wcscpy(pData, filePath);
    pData[len + 1] = L'\0';

    GlobalUnlock(hGlobal);

    SetClipboardData(CF_HDROP, hGlobal);
    CloseClipboard();
    return TRUE;
}

static void RunStorageGuardian(HWND hWnd, BOOL manualTrigger);

// -------------------------------------------------------------
// Real-Time Event-Driven Directory Watcher (ReadDirectoryChangesW)
// Detects finished MP4 recordings the instant they are written,
// without polling, without 90-second timeout, and without requiring
// the Snipping Tool editor window to close!
// -------------------------------------------------------------
static volatile BOOL g_bFolderWatcherRunning = TRUE;
static HANDLE g_hFolderWatcherThread = NULL;

static void OnNewRecordingDetected(const WCHAR* filePath) {
    // Wait until Snipping Tool finishes writing file and releases the lock
    BOOL fileReady = FALSE;
    for (int retries = 0; retries < 25; retries++) {
        if (g_hStopEvent && WaitForSingleObject(g_hStopEvent, 350) == WAIT_OBJECT_0) {
            return; // Instant clean shutdown
        } else if (!g_hStopEvent) {
            Sleep(350);
        }
        HANDLE hCheck = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hCheck != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(hCheck, &sz) && sz.QuadPart > 1024) {
                CloseHandle(hCheck);
                fileReady = TRUE;
                break;
            }
            CloseHandle(hCheck);
        }
    }
    if (!fileReady) return;

    static WCHAR s_lastCopied[MAX_PATH] = { 0 };
    if (_wcsicmp(s_lastCopied, filePath) == 0) return; // avoid duplicate
    wcsncpy(s_lastCopied, filePath, MAX_PATH - 1);
    s_lastCopied[MAX_PATH - 1] = L'\0';

    if (g_AutoCopyToClipboard) {
        if (CopyFileToClipboard(filePath)) {
            NOTIFYICONDATAW nid = { sizeof(nid) };
            nid.hWnd = g_hWndMain;
            nid.uID = ID_TRAY_APP_ICON;
            nid.uFlags = NIF_INFO;
            swprintf(nid.szInfoTitle, 64, L"High Snipping Frames");
            swprintf(nid.szInfo, 256, L"Recording copied to clipboard!\nReady to paste (Ctrl+V) directly into Discord or Slack.");
            nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
        }
    }

    if (g_EnableStorageGuardian && g_hWndMain) {
        PostMessageW(g_hWndMain, WM_COMMAND, IDM_STORAGE_AUTO, 0);
    }
}

static DWORD WINAPI FolderWatcherThreadProc(LPVOID lpParam) {
    WCHAR recDir[MAX_PATH];
    if (!GetRecordingsFolder(recDir, MAX_PATH)) return 0;

    HANDLE hDir = CreateFileW(
        recDir,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );

    if (hDir == INVALID_HANDLE_VALUE) return 0;

    HANDLE hIoEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!hIoEvent) {
        CloseHandle(hDir);
        return 0;
    }

    BYTE buffer[4096];
    OVERLAPPED ov = { 0 };
    ov.hEvent = hIoEvent;

    while (g_bFolderWatcherRunning) {
        ResetEvent(hIoEvent);
        DWORD bytesReturned = 0;
        BOOL issued = ReadDirectoryChangesW(
            hDir,
            buffer,
            sizeof(buffer),
            FALSE, // no subtree
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned,
            &ov,
            NULL
        );

        if (!issued && GetLastError() != ERROR_IO_PENDING) {
            if (g_hStopEvent && WaitForSingleObject(g_hStopEvent, 1000) == WAIT_OBJECT_0) break;
            continue;
        }

        HANDLE waitHandles[2] = { g_hStopEvent, hIoEvent };
        DWORD waitCount = g_hStopEvent ? 2 : 1;
        DWORD waitRes = g_hStopEvent
            ? WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE)
            : WaitForSingleObject(hIoEvent, INFINITE);

        if (g_hStopEvent && waitRes == WAIT_OBJECT_0) {
            CancelIo(hDir);
            break;
        }

        if (!GetOverlappedResult(hDir, &ov, &bytesReturned, FALSE) || bytesReturned == 0) {
            continue;
        }

        FILE_NOTIFY_INFORMATION* pNotify = (FILE_NOTIFY_INFORMATION*)buffer;
        while (pNotify) {
            if (pNotify->Action != FILE_ACTION_REMOVED &&
                pNotify->Action != FILE_ACTION_RENAMED_OLD_NAME &&
                pNotify->FileNameLength > 0) {
                WCHAR fileName[MAX_PATH] = { 0 };
                int cch = pNotify->FileNameLength / sizeof(WCHAR);
                if (cch < MAX_PATH) {
                    wcsncpy(fileName, pNotify->FileName, cch);
                    fileName[cch] = L'\0';
                    WCHAR* ext = wcsrchr(fileName, L'.');
                    if (ext && _wcsicmp(ext, L".mp4") == 0) {
                        WCHAR fullPath[MAX_PATH];
                        swprintf(fullPath, MAX_PATH, L"%ls\\%ls", recDir, fileName);
                        OnNewRecordingDetected(fullPath);
                        if (!g_bFolderWatcherRunning) break;
                    }
                }
            }
            if (pNotify->NextEntryOffset == 0) break;
            pNotify = (FILE_NOTIFY_INFORMATION*)((BYTE*)pNotify + pNotify->NextEntryOffset);
        }
    }

    CloseHandle(hIoEvent);
    CloseHandle(hDir);
    return 0;
}

static BOOL IsModuleLoaded(DWORD pid, const WCHAR* modName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    BOOL found = FALSE;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, modName) == 0) {
                found = TRUE;
                break;
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
    return found;
}

static BOOL InjectDLL(DWORD pid, const WCHAR* dllPath) {
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) return FALSE;

    DWORD accessRights = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | 
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE hProcess = OpenProcess(accessRights, FALSE, pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }
    if (!hProcess) return FALSE;

    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(WCHAR);
    LPVOID pRemoteBuf = VirtualAllocEx(hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteBuf) {
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pRemoteBuf, dllPath, pathBytes, NULL)) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE pLoadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLibraryW) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, pLoadLibraryW, pRemoteBuf, 0, NULL);
    if (!hThread) {
        VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return FALSE;
    }

    WaitForSingleObject(hThread, 3000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);

    VirtualFreeEx(hProcess, pRemoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    return (exitCode != 0 && exitCode != 0xC0000409);
}

// Handle tracking for zero-polling idle monitoring
static DWORD  g_InjectedPID = 0;
static HANDLE g_hInjectedProc = NULL;

static void ScanAndInject(const WCHAR* dllPath) {
    if (g_hInjectedProc != NULL) {
        DWORD waitRes = WaitForSingleObject(g_hInjectedProc, 0);
        if (waitRes == WAIT_TIMEOUT) {
            g_LastInjectError = FALSE;
            return; // Snipping Tool is running and already hooked
        }
        CloseHandle(g_hInjectedProc);
        g_hInjectedProc = NULL;
        g_InjectedPID = 0;
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    BOOL foundAny = FALSE;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"SnippingTool.exe") == 0) {
                foundAny = TRUE;
                if (!IsModuleLoaded(pe.th32ProcessID, L"SnippingToolHighFPS.dll")) {
                    // Self-heal AppContainer read+execute ACLs in Portable Mode before injection
                    EnsureAppContainerAccess(dllPath, GENERIC_READ | GENERIC_EXECUTE);
                    if (g_IniPath[0]) EnsureAppContainerAccess(g_IniPath, GENERIC_READ);

                    if (InjectDLL(pe.th32ProcessID, dllPath)) {
                        g_InjectedPID = pe.th32ProcessID;
                        g_hInjectedProc = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID);
                        g_LastInjectError = FALSE;
                        break;
                    } else {
                        g_LastInjectError = TRUE;
                    }
                } else {
                    g_InjectedPID = pe.th32ProcessID;
                    g_hInjectedProc = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID);
                    g_LastInjectError = FALSE;
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    if (!foundAny) g_LastInjectError = FALSE;
    CloseHandle(hSnap);
}

static DWORD WINAPI WatcherThreadProc(LPVOID lpParam) {
    // Gentle startup grace period: Windows measures CPU usage during logon
    if (g_hStopEvent && WaitForSingleObject(g_hStopEvent, 2500) == WAIT_OBJECT_0) {
        return 0;
    } else if (!g_hStopEvent) {
        Sleep(2500);
    }

    WCHAR dllPath[MAX_PATH];
    LocateDllPath(dllPath);
    EnsureAppContainerAccess(dllPath, GENERIC_READ | GENERIC_EXECUTE);
    if (g_IniPath[0]) EnsureAppContainerAccess(g_IniPath, GENERIC_READ);

    while (g_bWatcherRunning) {
        ScanAndInject(dllPath);
        DWORD sleepMs = (g_hInjectedProc != NULL) ? 1200 : 800;
        if (g_hStopEvent) {
            if (WaitForSingleObject(g_hStopEvent, sleepMs) == WAIT_OBJECT_0) break;
        } else {
            Sleep(sleepMs);
        }
    }

    if (g_hInjectedProc) {
        CloseHandle(g_hInjectedProc);
        g_hInjectedProc = NULL;
    }
    return 0;
}

// Read Settings from INI
static void LoadSettings() {
    LocateIniPath();
    if (g_IniPath[0]) {
        EnsureAppContainerAccess(g_IniPath, GENERIC_READ);
    }

    g_CurrentFPS = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 144, g_IniPath);
    g_QualityLevel = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 0, g_IniPath); // Balanced default
    if (g_QualityLevel > 2) g_QualityLevel = 0;

    int zoomPercent = GetPrivateProfileIntW(L"GUI", L"ZoomPercent", 100, g_IniPath);
    if (zoomPercent >= 80 && zoomPercent <= 200) {
        g_ZoomFactor = (float)zoomPercent / 100.0f;
    }
    g_AutoStart = IsAutoStartEnabled();

    // Storage Guardian Settings
    g_EnableStorageGuardian = GetPrivateProfileIntW(L"StorageGuardian", L"EnableStorageGuardian", 1, g_IniPath);
    g_MaxFolderSizeGB       = GetPrivateProfileIntW(L"StorageGuardian", L"MaxFolderSizeGB", 15, g_IniPath);
    g_TransferDays          = GetPrivateProfileIntW(L"StorageGuardian", L"TransferDays", 0, g_IniPath);
    GetPrivateProfileStringW(L"StorageGuardian", L"TransferFolder", L"", g_TransferFolder, MAX_PATH, g_IniPath);
    g_AutoCleanDays         = GetPrivateProfileIntW(L"StorageGuardian", L"AutoCleanDays", 0, g_IniPath);

    g_EnableGlobalHotkey    = GetPrivateProfileIntW(L"Settings", L"EnableGlobalHotkey", 1, g_IniPath);
    g_AutoCopyToClipboard   = GetPrivateProfileIntW(L"Settings", L"AutoCopyToClipboard", 1, g_IniPath);
}

// Write Settings to INI and verify write succeeded
static void SaveSettings() {
    WCHAR fpsStr[16];
    swprintf(fpsStr, 16, L"%u", g_CurrentFPS);
    BOOL ok1 = WritePrivateProfileStringW(L"Settings", L"TargetFPS", fpsStr, g_IniPath);

    WCHAR qualStr[16];
    swprintf(qualStr, 16, L"%d", g_QualityLevel);
    BOOL ok2 = WritePrivateProfileStringW(L"Settings", L"BitrateMultiplier", qualStr, g_IniPath);

    int zoomPercent = (int)(g_ZoomFactor * 100.0f + 0.5f);
    WCHAR zoomStr[16];
    swprintf(zoomStr, 16, L"%d", zoomPercent);
    BOOL ok3 = WritePrivateProfileStringW(L"GUI", L"ZoomPercent", zoomStr, g_IniPath);

    if (ok1 && g_IniPath[0]) {
        EnsureAppContainerAccess(g_IniPath, GENERIC_READ);
    }

    SetAutoStart(g_AutoStart);
    g_LastSavedSuccess = (ok1 && ok2 && ok3);
    g_LastSavedTick = GetTickCount();

    UpdateTrayTooltip();
}

static void ApplyZoomWindowSize(HWND hWnd) {
    int newWidth  = (int)(BASE_WIDTH * g_ZoomFactor + 0.5f);
    int newHeight = (int)(BASE_HEIGHT * g_ZoomFactor + 0.5f);

    RECT rcWin, rcClient;
    GetWindowRect(hWnd, &rcWin);
    GetClientRect(hWnd, &rcClient);

    int borderW = (rcWin.right - rcWin.left) - rcClient.right;
    int borderH = (rcWin.bottom - rcWin.top) - rcClient.bottom;

    SetWindowPos(hWnd, NULL, 0, 0, newWidth + borderW, newHeight + borderH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hWnd, NULL, TRUE);
}

static void RunStorageGuardian(HWND hWnd, BOOL manualTrigger) {
    if (!g_EnableStorageGuardian && !manualTrigger) return;

    WCHAR recDir[MAX_PATH];
    if (!GetRecordingsFolder(recDir, MAX_PATH)) return;

    WCHAR searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%ls\\*.mp4", recDir);

    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (manualTrigger) {
            MessageBoxW(hWnd, L"Screen Recordings folder is currently empty.\nNo cleanup or transfer required.",
                        L"Storage Guardian", MB_ICONINFORMATION);
        }
        return;
    }

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER uNow;
    uNow.LowPart = nowFt.dwLowDateTime;
    uNow.HighPart = nowFt.dwHighDateTime;

    ULONGLONG totalBytes = 0;
    DWORD totalCount = 0;
    DWORD transferredCount = 0;
    DWORD cleanedCount = 0;

    BOOL canTransfer = (g_TransferDays > 0 && g_TransferFolder[0] != L'\0');
    if (canTransfer) {
        if (GetFileAttributesW(g_TransferFolder) == INVALID_FILE_ATTRIBUTES) {
            SHCreateDirectoryExW(NULL, g_TransferFolder, NULL);
        }
        if (GetFileAttributesW(g_TransferFolder) == INVALID_FILE_ATTRIBUTES) {
            canTransfer = FALSE;
        }
    }

    do {
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        ULARGE_INTEGER uSize;
        uSize.LowPart = wfd.nFileSizeLow;
        uSize.HighPart = wfd.nFileSizeHigh;
        totalBytes += uSize.QuadPart;
        totalCount++;

        ULARGE_INTEGER uFile;
        uFile.LowPart = wfd.ftLastWriteTime.dwLowDateTime;
        uFile.HighPart = wfd.ftLastWriteTime.dwHighDateTime;

        ULONGLONG diff100ns = (uNow.QuadPart > uFile.QuadPart) ? (uNow.QuadPart - uFile.QuadPart) : 0;
        DWORD ageDays = (DWORD)(diff100ns / (10000000ULL * 3600 * 24));

        WCHAR srcFile[MAX_PATH];
        swprintf(srcFile, MAX_PATH, L"%ls\\%ls", recDir, wfd.cFileName);

        // 1. Transfer to Folder X if age >= TransferDays
        if (canTransfer && ageDays >= g_TransferDays) {
            WCHAR dstFile[MAX_PATH];
            swprintf(dstFile, MAX_PATH, L"%ls\\%ls", g_TransferFolder, wfd.cFileName);
            if (MoveFileExW(srcFile, dstFile, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
                transferredCount++;
                totalBytes -= uSize.QuadPart;
                continue;
            }
        }

        // 2. Auto-clean / delete if age >= AutoCleanDays
        if (g_AutoCleanDays > 0 && ageDays >= g_AutoCleanDays) {
            if (DeleteFileW(srcFile)) {
                cleanedCount++;
                totalBytes -= uSize.QuadPart;
            }
        }
    } while (FindNextFileW(hFind, &wfd));
    FindClose(hFind);

    double totalGB = (double)totalBytes / (1024.0 * 1024.0 * 1024.0);

    if (g_MaxFolderSizeGB > 0 && totalGB >= (double)g_MaxFolderSizeGB) {
        WCHAR warnMsg[256];
        swprintf(warnMsg, 256, L"Screen Recordings folder is currently %.1f GB (exceeds your %u GB limit).", totalGB, g_MaxFolderSizeGB);
        
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_APP_ICON;
        nid.uFlags = NIF_INFO;
        swprintf(nid.szInfoTitle, 64, L"High Snipping Frames - Storage Warning");
        wcscpy(nid.szInfo, warnMsg);
        nid.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    } else if (transferredCount > 0 || cleanedCount > 0) {
        WCHAR summaryMsg[256];
        swprintf(summaryMsg, 256, L"Transferred %u recording(s) and cleaned %u old file(s).", transferredCount, cleanedCount);
        
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_APP_ICON;
        nid.uFlags = NIF_INFO;
        swprintf(nid.szInfoTitle, 64, L"High Snipping Frames - Storage Guardian");
        wcscpy(nid.szInfo, summaryMsg);
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    if (manualTrigger) {
        WCHAR diagMsg[512];
        swprintf(diagMsg, 512,
                 L"Storage Guardian Status Report:\n\n"
                 L"• Current Folder Size: %.2f GB (%u videos)\n"
                 L"• Size Threshold: %u GB %s\n"
                 L"• Transferred to Archive: %u video(s)%s\n"
                 L"• Cleaned: %u video(s)%s\n\n"
                 L"Custom transfer and cleanup rules can be set in SnippingToolHighFPS.ini",
                 totalGB, totalCount,
                 g_MaxFolderSizeGB, (totalGB >= g_MaxFolderSizeGB && g_MaxFolderSizeGB > 0) ? L"[EXCEEDED]" : L"[OK]",
                 transferredCount, (canTransfer ? L"" : L" (Disabled)"),
                 cleanedCount, (g_AutoCleanDays > 0 ? L"" : L" (Disabled)"));
        MessageBoxW(hWnd, diagMsg, L"High Snipping Frames - Storage Guardian", MB_ICONINFORMATION);
    }
}

static void OpenRecordingsFolder() {
    WCHAR path[MAX_PATH];
    if (GetRecordingsFolder(path, MAX_PATH)) {
        ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void LaunchSnippingTool() {
    // 1. Direct Video Capture Protocol: Directly summon the video recording area
    HINSTANCE hInst = ShellExecuteW(NULL, L"open", L"ms-screenclip://capture/video", NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hInst > 32) return;

    // 2. Generic screen clip protocol
    hInst = ShellExecuteW(NULL, L"open", L"ms-screenclip:", NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hInst > 32) return;

    // 3. Fallback to standard executable launch
    hInst = ShellExecuteW(NULL, L"open", L"snippingtool.exe", NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hInst > 32) return;

    WCHAR localApp[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH)) {
        WCHAR aliasPath[MAX_PATH];
        swprintf(aliasPath, MAX_PATH, L"%ls\\Microsoft\\WindowsApps\\SnippingTool.exe", localApp);
        if (GetFileAttributesW(aliasPath) != INVALID_FILE_ATTRIBUTES) {
            hInst = ShellExecuteW(NULL, L"open", aliasPath, NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hInst > 32) return;
        }
    }

    WCHAR installedExe[MAX_PATH];
    if (FindInstalledSnippingToolPath(installedExe, MAX_PATH)) {
        ShellExecuteW(NULL, L"open", installedExe, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void AnalyzeLatestRecording(HWND hWnd) {
    WCHAR recDir[MAX_PATH];
    if (!GetRecordingsFolder(recDir, MAX_PATH)) return;

    WCHAR searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%ls\\*.mp4", recDir);

    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) {
        MessageBoxW(hWnd, L"No recordings found in Screen Recordings.\nRecord a video with Snipping Tool first!",
                    L"High Snipping Frames", MB_ICONINFORMATION);
        return;
    }

    FILETIME latestTime = wfd.ftLastWriteTime;
    WCHAR latestFile[MAX_PATH];
    swprintf(latestFile, MAX_PATH, L"%ls\\%ls", recDir, wfd.cFileName);

    while (FindNextFileW(hFind, &wfd)) {
        if (CompareFileTime(&wfd.ftLastWriteTime, &latestTime) > 0) {
            latestTime = wfd.ftLastWriteTime;
            swprintf(latestFile, MAX_PATH, L"%ls\\%ls", recDir, wfd.cFileName);
        }
    }
    FindClose(hFind);

    WCHAR cmd[1024];
    swprintf(cmd, 1024, L"ffprobe.exe -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate,nb_frames,duration,width,height,bit_rate -of default=noprint_wrappers=1:nokey=0 \"%ls\"", latestFile);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    char buffer[2048] = { 0 };
    if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        WaitForSingleObject(pi.hProcess, 3000);
        DWORD bytesRead = 0;
        ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRead);
    } else {
        CloseHandle(hWrite);
        CloseHandle(hRead);
        MessageBoxW(hWnd,
            L"ffprobe.exe was not found in your system PATH.\n\n"
            L"To enable instant video framerate verification:\n"
            L"1. Run 'winget install Gyan.FFmpeg' in PowerShell, or\n"
            L"2. Place ffprobe.exe in the same folder as this application.",
            L"High Snipping Frames - Video Verification", MB_ICONINFORMATION);
        return;
    }

    WCHAR msg[4096];
    WCHAR* pShortName = wcsrchr(latestFile, L'\\');
    swprintf(msg, 4096, L"Latest Video:\n%ls\n\nVerified Stream Analysis:\n%hs",
             pShortName ? (pShortName + 1) : latestFile, buffer);
    MessageBoxW(hWnd, msg, L"High Snipping Frames - Video Verification", MB_ICONINFORMATION);
}

// -------------------------------------------------------------
// UI Rendering & GDI+ Controls
// -------------------------------------------------------------
static void AddRoundedRectangle(GraphicsPath& path, RectF rect, float radius) {
    float diameter = radius * 2.0f;
    if (diameter > rect.Width) diameter = rect.Width;
    if (diameter > rect.Height) diameter = rect.Height;

    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRoundedRect(Graphics& g, Brush& brush, RectF rect, float radius) {
    GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);
    g.FillPath(&brush, &path);
}

static void DrawRoundedRect(Graphics& g, Pen& pen, RectF rect, float radius) {
    GraphicsPath path;
    AddRoundedRectangle(path, rect, radius);
    g.DrawPath(&pen, &path);
}

static void EnsureGdiplus() {
    if (g_gdiplusToken == 0) {
        GdiplusStartupInput gsi;
        GdiplusStartup(&g_gdiplusToken, &gsi, NULL);
    }
}

static BOOL GetLivePatchStatus(DWORD snipPid, int* pApplied, int* pExpected) {
    if (!snipPid || !g_IniPath[0]) return FALSE;
    WCHAR statusPath[MAX_PATH];
    wcscpy(statusPath, g_IniPath);
    WCHAR* sl = wcsrchr(statusPath, L'\\');
    if (!sl) return FALSE;
    wcscpy(sl + 1, L"patch_status.ini");
    DWORD pid = GetPrivateProfileIntW(L"Status", L"SnippingPID", 0, statusPath);
    if (pid != snipPid) return FALSE;
    if (pApplied) *pApplied = GetPrivateProfileIntW(L"Status", L"AppliedPatches", 0, statusPath);
    if (pExpected) *pExpected = GetPrivateProfileIntW(L"Status", L"ExpectedPatches", 4, statusPath);
    return TRUE;
}

static void RenderUI(Graphics& g, int clientWidth, int clientHeight) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    g.ScaleTransform(g_ZoomFactor, g_ZoomFactor);

    Color clrCanvas(0x1F, 0x1F, 0x1F);
    Color clrHeader(0x27, 0x27, 0x27);
    Color clrCard(0x2B, 0x2B, 0x2B);
    Color clrCardHover(0x32, 0x32, 0x32);
    Color clrCardBorder(0x38, 0x38, 0x38);
    Color clrAccent(0x00, 0x78, 0xD4);
    Color clrAccentHover(0x10, 0x84, 0xD9);
    Color clrAccentLight(0x60, 0xCD, 0xFF);
    Color clrTextWhite(0xFF, 0xFF, 0xFF);
    Color clrTextSub(0xDF, 0xDF, 0xDF);
    Color clrTextDim(0x9E, 0x9E, 0x9E);
    Color clrGreen(0x6C, 0xC6, 0x44);

    SolidBrush brCanvas(clrCanvas);
    SolidBrush brHeader(clrHeader);
    SolidBrush brCard(clrCard);
    SolidBrush brAccent(clrAccent);
    SolidBrush brAccentLight(clrAccentLight);
    SolidBrush brTextWhite(clrTextWhite);
    SolidBrush brTextSub(clrTextSub);
    SolidBrush brTextDim(clrTextDim);
    SolidBrush brGreen(clrGreen);

    Pen penBorder(clrCardBorder, 1.0f);
    Pen penAccent(clrAccentLight, 1.5f);

    FontFamily ff(L"Segoe UI");
    Font fontHeroNum(&ff, 28, FontStyleBold, UnitPixel);
    Font fontHeroTitle(&ff, 15, FontStyleBold, UnitPixel);
    Font fontTitle(&ff, 15, FontStyleBold, UnitPixel);
    Font fontSection(&ff, 11, FontStyleBold, UnitPixel);
    Font fontBody(&ff, 12, FontStyleRegular, UnitPixel);
    Font fontBodyBold(&ff, 12, FontStyleBold, UnitPixel);
    Font fontSmall(&ff, 11, FontStyleRegular, UnitPixel);
    Font fontBadge(&ff, 9, FontStyleBold, UnitPixel);

    StringFormat sfCenter;
    sfCenter.SetAlignment(StringAlignmentCenter);
    sfCenter.SetLineAlignment(StringAlignmentCenter);

    StringFormat sfLeft;
    sfLeft.SetAlignment(StringAlignmentNear);
    sfLeft.SetLineAlignment(StringAlignmentCenter);

    StringFormat sfRight;
    sfRight.SetAlignment(StringAlignmentFar);
    sfRight.SetLineAlignment(StringAlignmentCenter);

    // 1. Fill Canvas
    g.FillRectangle(&brCanvas, 0.0f, 0.0f, (float)BASE_WIDTH, (float)BASE_HEIGHT);

    // 2. Top Header Bar
    RectF rcHeader(0.0f, 0.0f, (float)BASE_WIDTH, 56.0f);
    g.FillRectangle(&brHeader, rcHeader);
    Pen penHLine(Color(0x33, 0x33, 0x33), 1.0f);
    g.DrawLine(&penHLine, 0.0f, 56.0f, (float)BASE_WIDTH, 56.0f);

    HICON hHeaderIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (hHeaderIcon) {
        Bitmap* pBmpIcon = Bitmap::FromHICON(hHeaderIcon);
        if (pBmpIcon) {
            g.DrawImage(pBmpIcon, 16.0f, 12.0f, 32.0f, 32.0f);
            delete pBmpIcon;
        }
        DestroyIcon(hHeaderIcon);
    }

    g.DrawString(L"High Snipping Frames", -1, &fontTitle, PointF(56.0f, 18.0f), &brTextWhite);

    // Top Right Version Badge
    RectF rcTopBadge(BASE_WIDTH - 96.0f, 17.0f, 78.0f, 22.0f);
    SolidBrush brBadge(Color(0x33, 0x33, 0x33));
    FillRoundedRect(g, brBadge, rcTopBadge, 4.0f);
    DrawRoundedRect(g, penBorder, rcTopBadge, 4.0f);
    g.DrawString(L"v1.2 PRO", -1, &fontBadge, rcTopBadge, &sfCenter, &brAccentLight);

    // 3. HERO CARD: Prominent Live Target Framerate Display
    RectF rcHero(16.0f, 70.0f, BASE_WIDTH - 32.0f, 84.0f);
    SolidBrush brHeroBg(Color(0x27, 0x27, 0x27));
    FillRoundedRect(g, brHeroBg, rcHero, 10.0f);
    Pen penHero(Color(0x00, 0x78, 0xD4), 1.2f);
    DrawRoundedRect(g, penHero, rcHero, 10.0f);

    WCHAR fpsMainStr[32];
    UINT32 maxRefresh = GetMaxDisplayRefresh();
    if (g_CurrentFPS == 0) {
        swprintf(fpsMainStr, 32, L"AUTO (%u FPS)", maxRefresh);
    } else {
        swprintf(fpsMainStr, 32, L"%u FPS", g_CurrentFPS);
    }

    g.DrawString(fpsMainStr, -1, &fontHeroNum, PointF(30.0f, 80.0f), &brTextWhite);
    g.DrawString(L"ACTIVE RECORDING TARGET", -1, &fontSection, PointF(32.0f, 120.0f), &brAccentLight);

    DWORD snipPid = FindProcessPID(L"SnippingTool.exe");
    int appliedPatches = 0, expectedPatches = 4;
    BOOL hasPatchStatus = GetLivePatchStatus(snipPid, &appliedPatches, &expectedPatches);

    WCHAR snipStatusStr[64];
    Color clrDot = clrGreen;
    if (snipPid == 0) {
        wcscpy(snipStatusStr, L"Idle");
    } else if (g_LastInjectError) {
        wcscpy(snipStatusStr, L"Injection Blocked");
        clrDot = Color(0xFF, 0x9F, 0x0A);
    } else if (hasPatchStatus && expectedPatches > 0) {
        if (appliedPatches == expectedPatches) {
            swprintf(snipStatusStr, 64, L"Hooked (%d/%d Patches)", appliedPatches, expectedPatches);
        } else {
            swprintf(snipStatusStr, 64, L"Partial (%d/%d Patches)", appliedPatches, expectedPatches);
            clrDot = Color(0xFF, 0x9F, 0x0A);
        }
    } else {
        wcscpy(snipStatusStr, L"Hooked & Ready");
    }

    RectF rcHeroRight(BASE_WIDTH - 210.0f, 80.0f, 180.0f, 64.0f);
    WCHAR sysInfo[128];
    swprintf(sysInfo, 128, L"Display: %u Hz\nEngine: Active (%ls)\nSnipping Tool: %ls",
             maxRefresh,
             g_bPortableMode ? L"Portable" : L"Unified",
             snipStatusStr);
    g.DrawString(sysInfo, -1, &fontSmall, rcHeroRight, &sfLeft, &brTextDim);

    SolidBrush brDot(clrDot);
    g.FillEllipse(&brDot, BASE_WIDTH - 222.0f, 96.0f, 6.0f, 6.0f);

    // 4. SECTION: Target Framerate Selection
    g.DrawString(L"SELECT TARGET FRAMERATE", -1, &fontSection, PointF(18.0f, 168.0f), &brTextDim);

    float fpsX = 16.0f;
    float fpsY = 188.0f;
    float pillW = (BASE_WIDTH - 32.0f - (g_NumFpsOptions - 1) * 6.0f) / g_NumFpsOptions;
    float pillH = 40.0f;

    for (int i = 0; i < g_NumFpsOptions; i++) {
        RectF rcPill(fpsX + i * (pillW + 6.0f), fpsY, pillW, pillH);
        BOOL isSelected = (g_CurrentFPS == g_FpsOptions[i]);
        BOOL isHovered  = (g_HoveredHitId == HIT_FPS_PRESET && g_HoveredData == i);

        if (isSelected) {
            SolidBrush brSel(clrAccent);
            FillRoundedRect(g, brSel, rcPill, 6.0f);
            Pen penSel(clrAccentLight, 1.2f);
            DrawRoundedRect(g, penSel, rcPill, 6.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBodyBold, rcPill, &sfCenter, &brTextWhite);
        } else {
            Color cPill = isHovered ? clrCardHover : clrCard;
            SolidBrush brPill(cPill);
            FillRoundedRect(g, brPill, rcPill, 6.0f);
            Pen penPill(isHovered ? clrAccentLight : clrCardBorder, 1.0f);
            DrawRoundedRect(g, penPill, rcPill, 6.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBody, rcPill, &sfCenter, isHovered ? &brTextWhite : &brTextSub);
        }
    }

    // 5. SECTION: Bitrate & Quality Setting
    g.DrawString(L"RECORDING BITRATE QUALITY (OPTIMIZED FILE SIZES)", -1, &fontSection, PointF(18.0f, 244.0f), &brTextDim);

    float qY = 264.0f;
    float qW = (BASE_WIDTH - 32.0f - 2 * 8.0f) / 3.0f;
    float qH = 58.0f;

    for (int i = 0; i < g_NumQualityOptions; i++) {
        RectF rcQ(16.0f + i * (qW + 8.0f), qY, qW, qH);
        BOOL isSelected = (g_QualityLevel == i);
        BOOL isHovered  = (g_HoveredHitId == HIT_QUALITY_PRESET && g_HoveredData == i);

        Color cBg = isSelected ? Color(0x23, 0x33, 0x48) : (isHovered ? clrCardHover : clrCard);
        SolidBrush brQ(cBg);
        FillRoundedRect(g, brQ, rcQ, 8.0f);
        Pen penQ(isSelected ? clrAccentLight : (isHovered ? Color(0x48, 0x48, 0x48) : clrCardBorder), isSelected ? 1.5f : 1.0f);
        DrawRoundedRect(g, penQ, rcQ, 8.0f);

        RectF rcQTitle(rcQ.X + 10.0f, rcQ.Y + 8.0f, rcQ.Width - 20.0f, 18.0f);
        RectF rcQDesc(rcQ.X + 10.0f, rcQ.Y + 28.0f, rcQ.Width - 20.0f, 24.0f);
        g.DrawString(g_QualityLabels[i], -1, &fontBodyBold, rcQTitle, &sfLeft, isSelected ? &brAccentLight : &brTextWhite);
        g.DrawString(g_QualityDescs[i], -1, &fontSmall, rcQDesc, &sfLeft, &brTextDim);
    }

    // 6. SECTION: Auto-Start Checkbox Card
    RectF rcAuto(16.0f, 336.0f, BASE_WIDTH - 32.0f, 40.0f);
    BOOL isAutoHover = (g_HoveredHitId == HIT_AUTOSTART);
    SolidBrush brAuto(isAutoHover ? clrCardHover : clrCard);
    FillRoundedRect(g, brAuto, rcAuto, 8.0f);
    DrawRoundedRect(g, penBorder, rcAuto, 8.0f);

    RectF rcBox(28.0f, 347.0f, 18.0f, 18.0f);
    if (g_AutoStart) {
        FillRoundedRect(g, brAccent, rcBox, 4.0f);
        Pen penCheck(Color(0xFF, 0xFF, 0xFF), 2.0f);
        PointF ptC[] = { { 32.0f, 356.0f }, { 36.0f, 360.0f }, { 42.0f, 351.0f } };
        g.DrawLines(&penCheck, ptC, 3);
    } else {
        Pen penBox(clrTextDim, 1.2f);
        DrawRoundedRect(g, penBox, rcBox, 4.0f);
    }
    g.DrawString(L"Start High Snipping Frames automatically with Windows", -1, &fontBody, PointF(54.0f, 346.0f), &brTextWhite);

    // 7. PRIMARY ACTIONS
    RectF rcBtnRec(16.0f, 392.0f, 150.0f, 42.0f);
    BOOL isRecHover = (g_HoveredHitId == HIT_BTN_RECORD);
    SolidBrush brBtnRec(isRecHover ? clrAccentHover : clrAccent);
    FillRoundedRect(g, brBtnRec, rcBtnRec, 6.0f);
    Pen penBtnRec(clrAccentLight, 1.0f);
    DrawRoundedRect(g, penBtnRec, rcBtnRec, 6.0f);
    g.DrawString(L"Record Screen", -1, &fontBodyBold, rcBtnRec, &sfCenter, &brTextWhite);

    RectF rcBtnFolder(174.0f, 392.0f, 140.0f, 42.0f);
    BOOL isFolderHover = (g_HoveredHitId == HIT_BTN_FOLDER);
    SolidBrush brFolder(isFolderHover ? clrCardHover : clrCard);
    FillRoundedRect(g, brFolder, rcBtnFolder, 6.0f);
    DrawRoundedRect(g, isFolderHover ? penAccent : penBorder, rcBtnFolder, 6.0f);
    g.DrawString(L"Open Videos", -1, &fontBody, rcBtnFolder, &sfCenter, isFolderHover ? &brAccentLight : &brTextWhite);

    RectF rcBtnVerify(322.0f, 392.0f, BASE_WIDTH - 322.0f - 16.0f, 42.0f);
    BOOL isVerifyHover = (g_HoveredHitId == HIT_BTN_ANALYZE);
    SolidBrush brVerify(isVerifyHover ? clrCardHover : clrCard);
    FillRoundedRect(g, brVerify, rcBtnVerify, 6.0f);
    DrawRoundedRect(g, isVerifyHover ? penAccent : penBorder, rcBtnVerify, 6.0f);
    g.DrawString(L"Verify Video FPS", -1, &fontBody, rcBtnVerify, &sfCenter, isVerifyHover ? &brAccentLight : &brTextWhite);

    // 8. Quick Tips / Info Box
    RectF rcInfo(16.0f, 448.0f, BASE_WIDTH - 32.0f, 52.0f);
    SolidBrush brInfo(Color(0x24, 0x24, 0x24));
    FillRoundedRect(g, brInfo, rcInfo, 8.0f);
    DrawRoundedRect(g, penBorder, rcInfo, 8.0f);

    DWORD now = GetTickCount();
    if (g_LastSavedTick != 0 && (now - g_LastSavedTick) < 2500) {
        if (g_LastSavedSuccess) {
            SolidBrush brGreenText(clrGreen);
            g.DrawString(L"✓ Setting applied instantly! Snipping Tool will record with the new target framerate.",
                         -1, &fontSmall, rcInfo, &sfCenter, &brGreenText);
        } else {
            SolidBrush brErrText(Color(0xFF, 0x69, 0x61));
            g.DrawString(L"Could not write to configuration INI file! Check folder permissions.",
                         -1, &fontSmall, rcInfo, &sfCenter, &brErrText);
        }
    } else {
        WCHAR tipText[160];
        swprintf(tipText, 160, L"Tip: %ls launches video recording. Finished MP4s copy to clipboard automatically.",
                 g_szActiveHotkey);
        g.DrawString(tipText, -1, &fontSmall, rcInfo, &sfCenter, &brTextDim);
    }

    // 9. BOTTOM BAR: Zoom Controls & Footer
    RectF rcFooter(0.0f, (float)BASE_HEIGHT - 44.0f, (float)BASE_WIDTH, 44.0f);
    SolidBrush brFoot(Color(0x18, 0x18, 0x18));
    g.FillRectangle(&brFoot, rcFooter);
    Pen penFLine(Color(0x2E, 0x2E, 0x2E), 1.0f);
    g.DrawLine(&penFLine, 0.0f, rcFooter.Y, (float)BASE_WIDTH, rcFooter.Y);

    int zoomPct = (int)(g_ZoomFactor * 100.0f + 0.5f);
    g.DrawString(L"Hold Ctrl + Scroll wheel to zoom", -1, &fontSmall, PointF(18.0f, rcFooter.Y + 12.0f), &brTextDim);

    float zBtnY = rcFooter.Y + 8.0f;
    RectF rcZMinus(BASE_WIDTH - 150.0f, zBtnY, 28.0f, 26.0f);
    RectF rcZText(BASE_WIDTH - 118.0f, zBtnY, 56.0f, 26.0f);
    RectF rcZPlus(BASE_WIDTH - 58.0f, zBtnY, 28.0f, 26.0f);

    BOOL hMinus = (g_HoveredHitId == HIT_ZOOM_MINUS);
    BOOL hText  = (g_HoveredHitId == HIT_ZOOM_RESET);
    BOOL hPlus  = (g_HoveredHitId == HIT_ZOOM_PLUS);

    SolidBrush brZBtn(Color(0x28, 0x28, 0x28));
    SolidBrush brZHov(Color(0x38, 0x38, 0x38));

    FillRoundedRect(g, hMinus ? brZHov : brZBtn, rcZMinus, 4.0f);
    DrawRoundedRect(g, penBorder, rcZMinus, 4.0f);
    g.DrawString(L"-", -1, &fontBodyBold, rcZMinus, &sfCenter, &brTextWhite);

    FillRoundedRect(g, hText ? brZHov : brZBtn, rcZText, 4.0f);
    DrawRoundedRect(g, penBorder, rcZText, 4.0f);
    WCHAR pctStr[16];
    swprintf(pctStr, 16, L"%d%%", zoomPct);
    g.DrawString(pctStr, -1, &fontSmall, rcZText, &sfCenter, hText ? &brAccentLight : &brTextWhite);

    FillRoundedRect(g, hPlus ? brZHov : brZBtn, rcZPlus, 4.0f);
    DrawRoundedRect(g, penBorder, rcZPlus, 4.0f);
    g.DrawString(L"+", -1, &fontBodyBold, rcZPlus, &sfCenter, &brTextWhite);
}

static int HitTest(int mouseX, int mouseY, int* pData) {
    if (pData) *pData = -1;

    float lx = (float)mouseX / g_ZoomFactor;
    float ly = (float)mouseY / g_ZoomFactor;

    // Check FPS presets
    float fpsX = 16.0f;
    float fpsY = 188.0f;
    float pillW = (BASE_WIDTH - 32.0f - (g_NumFpsOptions - 1) * 6.0f) / g_NumFpsOptions;
    float pillH = 40.0f;

    for (int i = 0; i < g_NumFpsOptions; i++) {
        float x1 = fpsX + i * (pillW + 6.0f);
        if (lx >= x1 && lx <= x1 + pillW && ly >= fpsY && ly <= fpsY + pillH) {
            if (pData) *pData = i;
            return HIT_FPS_PRESET;
        }
    }

    // Check Quality presets
    float qY = 264.0f;
    float qW = (BASE_WIDTH - 32.0f - 2 * 8.0f) / 3.0f;
    float qH = 58.0f;
    for (int i = 0; i < g_NumQualityOptions; i++) {
        float x1 = 16.0f + i * (qW + 8.0f);
        if (lx >= x1 && lx <= x1 + qW && ly >= qY && ly <= qY + qH) {
            if (pData) *pData = i;
            return HIT_QUALITY_PRESET;
        }
    }

    // Check AutoStart
    if (lx >= 16.0f && lx <= BASE_WIDTH - 16.0f && ly >= 336.0f && ly <= 376.0f) {
        return HIT_AUTOSTART;
    }

    // Buttons
    if (lx >= 16.0f && lx <= 166.0f && ly >= 392.0f && ly <= 434.0f) return HIT_BTN_RECORD;
    if (lx >= 174.0f && lx <= 314.0f && ly >= 392.0f && ly <= 434.0f) return HIT_BTN_FOLDER;
    if (lx >= 322.0f && lx <= BASE_WIDTH - 16.0f && ly >= 392.0f && ly <= 434.0f) return HIT_BTN_ANALYZE;

    // Zoom buttons
    float zBtnY = (float)BASE_HEIGHT - 36.0f;
    if (lx >= BASE_WIDTH - 150.0f && lx <= BASE_WIDTH - 122.0f && ly >= zBtnY && ly <= zBtnY + 26.0f) return HIT_ZOOM_MINUS;
    if (lx >= BASE_WIDTH - 118.0f && lx <= BASE_WIDTH - 62.0f  && ly >= zBtnY && ly <= zBtnY + 26.0f) return HIT_ZOOM_RESET;
    if (lx >= BASE_WIDTH - 58.0f  && lx <= BASE_WIDTH - 30.0f  && ly >= zBtnY && ly <= zBtnY + 26.0f) return HIT_ZOOM_PLUS;

    return HIT_NONE;
}

static void InitDarkMode() {
    HMODULE hUxtheme = LoadLibraryW(L"uxtheme.dll");
    if (hUxtheme) {
        typedef BOOL (WINAPI *fnSetPreferredAppMode)(int);
        fnSetPreferredAppMode pSetMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
        if (pSetMode) {
            pSetMode(2); // ForceDark
        }
    }
}

static void UpdateTrayTooltip() {
    UINT32 maxRefresh = GetMaxDisplayRefresh();
    if (g_CurrentFPS == 0) {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (%u FPS)", maxRefresh);
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", g_CurrentFPS);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    UINT32 maxRefresh = GetMaxDisplayRefresh();

    // 1. Header Item
    WCHAR headerStr[128];
    if (g_CurrentFPS == 0) {
        swprintf(headerStr, 128, L"High Snipping Frames (Auto %u Hz)", maxRefresh);
    } else {
        swprintf(headerStr, 128, L"High Snipping Frames (%u FPS)", g_CurrentFPS);
    }
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, headerStr);
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open Control Center");
    SetMenuDefaultItem(hMenu, IDM_OPEN, FALSE);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 2. Framerate Selection Submenu
    HMENU hFpsMenu = CreatePopupMenu();
    WCHAR autoLabel[64];
    swprintf(autoLabel, 64, L"Auto (Match Display - %u Hz)", maxRefresh);
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 0)   ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_AUTO, autoLabel);
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 60)  ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_60,   L"60 FPS (Balanced)");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 90)  ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_90,   L"90 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 120) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_120,  L"120 FPS (High)");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 144) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_144,  L"144 FPS (Ultra)");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 165) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_165,  L"165 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 240) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_240,  L"240 FPS (Esports)");

    WCHAR curFpsStr[64];
    if (g_CurrentFPS == 0) {
        swprintf(curFpsStr, 64, L"Target Framerate (Auto %u Hz)", maxRefresh);
    } else {
        swprintf(curFpsStr, 64, L"Target Framerate (%u FPS)", g_CurrentFPS);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFpsMenu, curFpsStr);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 3. Quick Actions
    WCHAR recLabel[64];
    if (g_bHotkeyRegistered) {
        swprintf(recLabel, 64, L"Record Screen (%ls)", g_szActiveHotkey);
    } else {
        wcscpy(recLabel, L"Record Screen");
    }
    AppendMenuW(hMenu, MF_STRING, IDM_RECORD, recLabel);
    AppendMenuW(hMenu, MF_STRING, IDM_FOLDER,  L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_ANALYZE, L"Verify Latest Video");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 4. Utility / System
    AppendMenuW(hMenu, MF_STRING, IDM_REPAIR,        L"Verify Snipping Tool Integration");
    AppendMenuW(hMenu, MF_STRING, IDM_STORAGE_CHECK, L"Storage Guardian: Check Status");
    AppendMenuW(hMenu, MF_STRING, IDM_RESET_ZOOM,    L"Reset Zoom (100%)");
    AppendMenuW(hMenu, MF_STRING, IDM_RESTART_SVC,   L"Rescan Snipping Tool");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 5. Exit
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != 0 && msg == g_uMsgTaskbarCreated) {
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hWnd;
        g_nid.uID = ID_TRAY_APP_ICON;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
            DWORD corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

            SetTimer(hWnd, TIMER_STORAGE_GUARDIAN, 4 * 3600 * 1000, NULL);
            SetTimer(hWnd, TIMER_STORAGE_STARTUP, 10000, NULL);
            SetTimer(hWnd, TIMER_STATUS_REFRESH, 1500, NULL);
            break;
        }

        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                SetTimer(hWnd, TIMER_SLEEP_RECOVERY, 1500, NULL);
            }
            return TRUE;
        }

        case WM_DISPLAYCHANGE: {
            SetTimer(hWnd, TIMER_SLEEP_RECOVERY, 500, NULL);
            break;
        }

        case WM_TIMER: {
            if (wParam == TIMER_SLEEP_RECOVERY) {
                KillTimer(hWnd, TIMER_SLEEP_RECOVERY);
                UpdateTrayTooltip();
                WCHAR dllPath[MAX_PATH];
                LocateDllPath(dllPath);
                ScanAndInject(dllPath);
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (wParam == TIMER_STORAGE_GUARDIAN) {
                RunStorageGuardian(hWnd, FALSE);
            } else if (wParam == TIMER_STORAGE_STARTUP) {
                KillTimer(hWnd, TIMER_STORAGE_STARTUP);
                RunStorageGuardian(hWnd, FALSE);
            } else if (wParam == TIMER_STATUS_REFRESH) {
                if (IsWindowVisible(hWnd)) {
                    DWORD curPid = FindProcessPID(L"SnippingTool.exe");
                    int applied = -1, expected = 4;
                    if (curPid) GetLivePatchStatus(curPid, &applied, &expected);
                    BOOL toastExpired = (g_LastSavedTick != 0 && (GetTickCount() - g_LastSavedTick) >= 2500);
                    if (curPid != g_LastUiSnipPid || applied != g_LastUiAppliedPatches || toastExpired) {
                        g_LastUiSnipPid = curPid;
                        g_LastUiAppliedPatches = applied;
                        if (toastExpired) g_LastSavedTick = 0;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                }
            }
            break;
        }

        case WM_HOTKEY: {
            if (wParam == HOTKEY_RECORD_VIDEO) {
                LaunchSnippingTool();
            }
            break;
        }

        case WM_MOUSEWHEEL: {
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (zDelta > 0) {
                    if (g_ZoomFactor < 1.95f) {
                        g_ZoomFactor += 0.10f;
                        if (g_ZoomFactor > 2.0f) g_ZoomFactor = 2.0f;
                        ApplyZoomWindowSize(hWnd);
                        SaveSettings();
                    }
                } else if (zDelta < 0) {
                    if (g_ZoomFactor > 0.85f) {
                        g_ZoomFactor -= 0.10f;
                        if (g_ZoomFactor < 0.8f) g_ZoomFactor = 0.8f;
                        ApplyZoomWindowSize(hWnd);
                        SaveSettings();
                    }
                }
                return 0;
            }
            break;
        }

        case WM_KEYDOWN: {
            if (wParam == '0' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_ZoomFactor = 1.0f;
                ApplyZoomWindowSize(hWnd);
                SaveSettings();
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            int data = -1;
            int hit = HitTest(mx, my, &data);

            if (hit != g_HoveredHitId || data != g_HoveredData) {
                g_HoveredHitId = hit;
                g_HoveredData = data;
                InvalidateRect(hWnd, NULL, FALSE);
            }

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            break;
        }

        case WM_MOUSELEAVE: {
            if (g_HoveredHitId != HIT_NONE) {
                g_HoveredHitId = HIT_NONE;
                g_HoveredData = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        }

        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT && g_HoveredHitId != HIT_NONE) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);
            int data = -1;
            int hit = HitTest(mx, my, &data);

            if (hit == HIT_FPS_PRESET && data >= 0 && data < g_NumFpsOptions) {
                g_CurrentFPS = g_FpsOptions[data];
                SaveSettings();
                UpdateTrayTooltip();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_QUALITY_PRESET && data >= 0 && data < g_NumQualityOptions) {
                g_QualityLevel = data;
                SaveSettings();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_ALERT_REPAIR) {
                RepairSnippingToolAssociations(hWnd);
            } else if (hit == HIT_AUTOSTART) {
                g_AutoStart = !g_AutoStart;
                SetAutoStart(g_AutoStart);
                g_LastSavedSuccess = TRUE;
                g_LastSavedTick = GetTickCount();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_BTN_RECORD) {
                LaunchSnippingTool();
            } else if (hit == HIT_BTN_FOLDER) {
                OpenRecordingsFolder();
            } else if (hit == HIT_BTN_ANALYZE) {
                AnalyzeLatestRecording(hWnd);
            } else if (hit == HIT_ZOOM_MINUS) {
                if (g_ZoomFactor > 0.85f) {
                    g_ZoomFactor -= 0.10f;
                    if (g_ZoomFactor < 0.8f) g_ZoomFactor = 0.8f;
                    ApplyZoomWindowSize(hWnd);
                    SaveSettings();
                }
            } else if (hit == HIT_ZOOM_PLUS) {
                if (g_ZoomFactor < 1.95f) {
                    g_ZoomFactor += 0.10f;
                    if (g_ZoomFactor > 2.0f) g_ZoomFactor = 2.0f;
                    ApplyZoomWindowSize(hWnd);
                    SaveSettings();
                }
            } else if (hit == HIT_ZOOM_RESET) {
                g_ZoomFactor = 1.0f;
                ApplyZoomWindowSize(hWnd);
                SaveSettings();
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            if (w > 0 && h > 0) {
                EnsureGdiplus();
                Bitmap backBuffer(w, h, PixelFormat32bppARGB);
                Graphics g(&backBuffer);
                RenderUI(g, w, h);
                Graphics gScreen(hdc);
                gScreen.DrawImage(&backBuffer, 0, 0);
            }
            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDM_OPEN:
                    ShowWindow(hWnd, SW_SHOW);
                    SetForegroundWindow(hWnd);
                    break;

                case IDM_RECORD: LaunchSnippingTool(); break;
                case IDM_FOLDER: OpenRecordingsFolder(); break;
                case IDM_ANALYZE: AnalyzeLatestRecording(hWnd); break;
                case IDM_STORAGE_CHECK: RunStorageGuardian(hWnd, TRUE); break;
                case IDM_STORAGE_AUTO: RunStorageGuardian(hWnd, FALSE); break;
                case IDM_REPAIR: RepairSnippingToolAssociations(hWnd); break;

                case IDM_FPS_AUTO: g_CurrentFPS = 0;   SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_60:   g_CurrentFPS = 60;  SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_90:   g_CurrentFPS = 90;  SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_120:  g_CurrentFPS = 120; SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_144:  g_CurrentFPS = 144; SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_165:  g_CurrentFPS = 165; SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_240:  g_CurrentFPS = 240; SaveSettings(); UpdateTrayTooltip(); InvalidateRect(hWnd, NULL, FALSE); break;

                case IDM_RESET_ZOOM:
                    g_ZoomFactor = 1.0f;
                    ApplyZoomWindowSize(hWnd);
                    SaveSettings();
                    break;

                case IDM_RESTART_SVC: {
                    WCHAR dllPath[MAX_PATH];
                    LocateDllPath(dllPath);
                    ScanAndInject(dllPath);
                    MessageBoxW(hWnd, L"High Snipping Frames unified engine rescanned and verified.", L"High Snipping Frames", MB_ICONINFORMATION);
                    InvalidateRect(hWnd, NULL, FALSE);
                    break;
                }

                case IDM_EXIT:
                    Shell_NotifyIconW(NIM_DELETE, &g_nid);
                    DestroyWindow(hWnd);
                    break;
            }
            break;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hWnd);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
            }
            break;
        }

        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            KillTimer(hWnd, TIMER_STATUS_REFRESH);
            if (g_hStopEvent) SetEvent(g_hStopEvent);
            if (g_bWatcherRunning) {
                g_bWatcherRunning = FALSE;
                if (g_hWatcherThread) {
                    WaitForSingleObject(g_hWatcherThread, 1500);
                    CloseHandle(g_hWatcherThread);
                    g_hWatcherThread = NULL;
                }
            }
            if (g_bFolderWatcherRunning) {
                g_bFolderWatcherRunning = FALSE;
                if (g_hFolderWatcherThread) {
                    WaitForSingleObject(g_hFolderWatcherThread, 1500);
                    CloseHandle(g_hFolderWatcherThread);
                    g_hFolderWatcherThread = NULL;
                }
            }
            if (g_hStopEvent) {
                CloseHandle(g_hStopEvent);
                g_hStopEvent = NULL;
            }
            if (g_bHotkeyRegistered) {
                UnregisterHotKey(hWnd, HOTKEY_RECORD_VIDEO);
                g_bHotkeyRegistered = FALSE;
            }
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *fnSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
        fnSetProcessDpiAwarenessContext pSetDpi = 
            (fnSetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetDpi) {
            pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            typedef BOOL (WINAPI *fnSetProcessDPIAware)();
            fnSetProcessDPIAware pSetDpiLegacy = (fnSetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
            if (pSetDpiLegacy) pSetDpiLegacy();
        }
    }

    g_uMsgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    InitDarkMode();
    g_hInstance = hInstance;

    // Purge legacy IFEO hook if elevated
    HKEY hIfeoKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SnippingTool.exe",
        0, KEY_SET_VALUE, &hIfeoKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hIfeoKey, L"Debugger");
        RegCloseKey(hIfeoKey);
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HighSnippingFramesGUIMutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExist = FindWindowW(L"HighSnippingFramesGUIClass_v1", NULL);
        if (hExist) {
            if (IsIconic(hExist)) ShowWindow(hExist, SW_RESTORE);
            else ShowWindow(hExist, SW_SHOW);
            SetForegroundWindow(hExist);
        }
        if (hMutex) CloseHandle(hMutex);
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    LoadSettings();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HighSnippingFramesGUIClass_v1";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    int w = (int)(BASE_WIDTH * g_ZoomFactor + 0.5f);
    int h = (int)(BASE_HEIGHT * g_ZoomFactor + 0.5f);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hWndMain = CreateWindowExW(WS_EX_APPWINDOW, L"HighSnippingFramesGUIClass_v1",
                                 L"High Snipping Frames",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hWndMain) {
        if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    ApplyZoomWindowSize(g_hWndMain);

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hWndMain;
    g_nid.uID = ID_TRAY_APP_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    if (g_CurrentFPS == 0) {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (High FPS)");
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", g_CurrentFPS);
    }
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Register Global Instant Video Hotkey with fallback
    if (g_EnableGlobalHotkey && g_hWndMain) {
        g_bHotkeyRegistered = RegisterHotKey(g_hWndMain, HOTKEY_RECORD_VIDEO, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'R');
        if (!g_bHotkeyRegistered) {
            g_bHotkeyRegistered = RegisterHotKey(g_hWndMain, HOTKEY_RECORD_VIDEO, MOD_WIN | MOD_ALT, 'R');
        }
        if (g_bHotkeyRegistered) {
            wcscpy(g_szActiveHotkey, L"Win + Alt + R");
        } else {
            // Fallback to Ctrl + Alt + R if Win + Alt + R is locked by Xbox Game Bar
            g_bHotkeyRegistered = RegisterHotKey(g_hWndMain, HOTKEY_RECORD_VIDEO, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
            if (!g_bHotkeyRegistered) {
                g_bHotkeyRegistered = RegisterHotKey(g_hWndMain, HOTKEY_RECORD_VIDEO, MOD_CONTROL | MOD_ALT, 'R');
            }
            if (g_bHotkeyRegistered) {
                wcscpy(g_szActiveHotkey, L"Ctrl + Alt + R");
            }
        }
    }

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    // 1. Start background injection watcher thread
    g_bWatcherRunning = TRUE;
    g_hWatcherThread = CreateThread(NULL, 0, WatcherThreadProc, NULL, 0, NULL);

    // 2. Start real-time directory event watcher thread (instant clipboard copy)
    g_bFolderWatcherRunning = TRUE;
    g_hFolderWatcherThread = CreateThread(NULL, 0, FolderWatcherThreadProc, NULL, 0, NULL);

    if (lpCmdLine && wcsstr(lpCmdLine, L"--minimized")) {
        ShowWindow(g_hWndMain, SW_HIDE);
    } else {
        EnsureGdiplus();
        ShowWindow(g_hWndMain, SW_SHOW);
        UpdateWindow(g_hWndMain);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hStopEvent) {
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
    }
    if (hMutex) CloseHandle(hMutex);
    if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
    return 0;
}
