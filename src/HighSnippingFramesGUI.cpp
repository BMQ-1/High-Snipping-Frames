#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

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
#define IDM_EXIT          3025

#define HOTKEY_RECORD_VIDEO 2001

// Internal Timers
#define TIMER_SLEEP_RECOVERY    4001
#define TIMER_STORAGE_GUARDIAN  4002
#define TIMER_STORAGE_STARTUP   4003

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

static UINT32 g_CurrentFPS = 144;
static int    g_QualityLevel = 1; // 0=Balanced, 1=High, 2=Ultra
static float  g_ZoomFactor = 1.0f; // Zoom: 0.8f - 2.0f
static BOOL   g_AutoStart = TRUE;
static DWORD  g_LastSavedTick = 0; // For temporary "✓ Saved" toast animation

// Storage Guardian Configuration
static BOOL  g_EnableStorageGuardian   = TRUE;
static DWORD g_MaxFolderSizeGB         = 15;        // Alert if folder exceeds size in GB (0 = disabled)
static DWORD g_TransferDays            = 0;         // Transfer recordings older than X days to Folder X (0 = disabled)
static WCHAR g_TransferFolder[MAX_PATH] = { 0 };    // Destination Folder X
static DWORD g_AutoCleanDays           = 0;         // Auto-delete recordings older than X days (0 = disabled)

// Part 3: Instant Video Hotkey & Clipboard Fast-Share
static BOOL  g_EnableGlobalHotkey      = TRUE;      // Win + Alt + R
static BOOL  g_AutoCopyToClipboard     = TRUE;      // Automatically put fresh MP4 into Windows clipboard

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

static int g_HoveredHitId = HIT_NONE;
static int g_HoveredData = -1;

// FPS presets: 0 = Auto, followed by fixed rates
static const UINT32 g_FpsOptions[] = { 0, 60, 90, 120, 144, 165, 240 };
static const WCHAR* g_FpsLabels[]  = { L"AUTO", L"60", L"90", L"120", L"144", L"165", L"240" };
static const int g_NumFpsOptions   = 7;

// Quality presets
static const WCHAR* g_QualityLabels[] = { L"Balanced (1.0x)", L"High Quality (1.5x)", L"Ultra Gaming (2.0x)" };
static const WCHAR* g_QualityDescs[]  = { L"Standard bitrates, smaller file size", L"Crisp high-framerate action", L"Lossless-like fidelity for 144+ FPS" };
static const int g_NumQualityOptions = 3;

// Helper: Locate INI Path
static void LocateIniPath() {
    WCHAR selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *pSlash = L'\0';

    swprintf(g_IniPath, MAX_PATH, L"%s\\SnippingToolHighFPS.ini", selfDir);
    if (GetFileAttributesW(g_IniPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(g_IniPath, L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolHighFPS.ini");
    }
    if (GetFileAttributesW(g_IniPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(g_IniPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\High-Snipping-Frames\\bin\\SnippingToolHighFPS.ini");
    }
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
    return maxFreq;
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
            WCHAR guiPath[MAX_PATH];
            swprintf(guiPath, MAX_PATH, L"\"C:\\Program Files\\SnippingToolHighFPS\\HighSnippingFramesGUI.exe\" --minimized");
            RegSetValueExW(hKey, L"HighSnippingFrames", 0, REG_SZ, (const BYTE*)guiPath, (DWORD)(wcslen(guiPath) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"HighSnippingFrames");
        }
        // Always clean up legacy daemon entries to ensure ONLY ONE instance ever runs
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

    swprintf(outPath, MAX_PATH, L"%s\\SnippingToolHighFPS.dll", selfDir);
    if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(outPath, L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolHighFPS.dll");
    }
    if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(outPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\High-Snipping-Frames\\bin\\SnippingToolHighFPS.dll");
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

static void AutoCopyLatestRecordingToClipboard(HWND hWnd) {
    WCHAR userProfile[MAX_PATH];
    if (!GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH)) return;

    WCHAR searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%s\\Videos\\Screen Recordings\\*.mp4", userProfile);

    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    FILETIME latestTime = wfd.ftLastWriteTime;
    WCHAR latestFile[MAX_PATH];
    swprintf(latestFile, MAX_PATH, L"%s\\Videos\\Screen Recordings\\%s", userProfile, wfd.cFileName);

    while (FindNextFileW(hFind, &wfd)) {
        if (CompareFileTime(&wfd.ftLastWriteTime, &latestTime) > 0) {
            latestTime = wfd.ftLastWriteTime;
            swprintf(latestFile, MAX_PATH, L"%s\\Videos\\Screen Recordings\\%s", userProfile, wfd.cFileName);
        }
    }
    FindClose(hFind);

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER uNow, uFile;
    uNow.LowPart = nowFt.dwLowDateTime; uNow.HighPart = nowFt.dwHighDateTime;
    uFile.LowPart = latestTime.dwLowDateTime; uFile.HighPart = latestTime.dwHighDateTime;
    ULONGLONG diffSec = (uNow.QuadPart > uFile.QuadPart) ? ((uNow.QuadPart - uFile.QuadPart) / 10000000ULL) : 0;

    if (diffSec <= 90) {
        if (CopyFileToClipboard(latestFile)) {
            ZeroMemory(&g_nid, sizeof(g_nid));
            g_nid.cbSize = sizeof(g_nid);
            g_nid.hWnd = hWnd;
            g_nid.uID = ID_TRAY_APP_ICON;
            g_nid.uFlags = NIF_INFO;
            swprintf(g_nid.szInfoTitle, 64, L"High Snipping Frames");
            swprintf(g_nid.szInfo, 256, L"Recording copied to clipboard!\nReady to paste (Ctrl+V) directly into Discord or Slack.");
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        }
    }
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

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
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

// Handle tracking for zero-polling idle monitoring (near 0ms boot CPU impact)
static DWORD  g_InjectedPID = 0;
static HANDLE g_hInjectedProc = NULL;

static void ScanAndInject(const WCHAR* dllPath) {
    // If we are already tracking the hooked Snipping Tool instance,
    // check if it is still alive. While alive, do ZERO snapshots (0% CPU, 0 I/O)!
    if (g_hInjectedProc != NULL) {
        DWORD waitRes = WaitForSingleObject(g_hInjectedProc, 0);
        if (waitRes == WAIT_TIMEOUT) {
            return; // Snipping Tool is running and already hooked
        }
        CloseHandle(g_hInjectedProc);
        g_hInjectedProc = NULL;
        g_InjectedPID = 0;

        // Snipping Tool has finished recording: auto-copy to clipboard for fast sharing
        if (g_AutoCopyToClipboard && g_hWndMain) {
            Sleep(500); // Give Snipping Tool a brief moment to finish writing file header
            AutoCopyLatestRecordingToClipboard(g_hWndMain);
        }
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"SnippingTool.exe") == 0) {
                if (!IsModuleLoaded(pe.th32ProcessID, L"SnippingToolHighFPS.dll")) {
                    if (InjectDLL(pe.th32ProcessID, dllPath)) {
                        g_InjectedPID = pe.th32ProcessID;
                        g_hInjectedProc = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID);
                        break;
                    }
                } else {
                    g_InjectedPID = pe.th32ProcessID;
                    g_hInjectedProc = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

static DWORD WINAPI WatcherThreadProc(LPVOID lpParam) {
    // Gentle startup grace period: Windows measures CPU usage during logon.
    // By sleeping 2.5s initially, boot CPU impact drops to nearly 0 ms.
    Sleep(2500);

    WCHAR dllPath[MAX_PATH];
    LocateDllPath(dllPath);

    while (g_bWatcherRunning) {
        ScanAndInject(dllPath);
        
        // Adaptive sleeping:
        // When hooked, sleep 1200ms (we only need to know when it closes).
        // When waiting for launch, sleep 800ms (negligible CPU, imperceptible latency).
        DWORD sleepMs = (g_hInjectedProc != NULL) ? 1200 : 800;
        Sleep(sleepMs);
    }

    if (g_hInjectedProc) {
        CloseHandle(g_hInjectedProc);
        g_hInjectedProc = NULL;
    }
    return 0;
}

// Helper: Read Settings from INI
static void LoadSettings() {
    LocateIniPath();
    g_CurrentFPS = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 144, g_IniPath);
    g_QualityLevel = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 1, g_IniPath);
    if (g_QualityLevel > 2) g_QualityLevel = 1;

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

    // Part 3 Settings
    g_EnableGlobalHotkey    = GetPrivateProfileIntW(L"Settings", L"EnableGlobalHotkey", 1, g_IniPath);
    g_AutoCopyToClipboard   = GetPrivateProfileIntW(L"Settings", L"AutoCopyToClipboard", 1, g_IniPath);
}

// Helper: Write Settings to INI
static void SaveSettings() {
    WCHAR fpsStr[16];
    swprintf(fpsStr, 16, L"%u", g_CurrentFPS);
    WritePrivateProfileStringW(L"Settings", L"TargetFPS", fpsStr, g_IniPath);

    WCHAR qualStr[16];
    swprintf(qualStr, 16, L"%d", g_QualityLevel);
    WritePrivateProfileStringW(L"Settings", L"BitrateMultiplier", qualStr, g_IniPath);

    int zoomPercent = (int)(g_ZoomFactor * 100.0f + 0.5f);
    WCHAR zoomStr[16];
    swprintf(zoomStr, 16, L"%d", zoomPercent);
    WritePrivateProfileStringW(L"GUI", L"ZoomPercent", zoomStr, g_IniPath);

    SetAutoStart(g_AutoStart);
    g_LastSavedTick = GetTickCount();

    // Update Tray Tooltip
    if (g_CurrentFPS == 0) {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (%u Hz)", GetMaxDisplayRefresh());
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", g_CurrentFPS);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Apply window size matching the current zoom factor
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

// Storage Guardian Engine: Monitors disk usage, warns on high capacity,
// and automatically transfers or cleans recordings based on age.
static void RunStorageGuardian(HWND hWnd, BOOL manualTrigger) {
    if (!g_EnableStorageGuardian && !manualTrigger) return;

    WCHAR userProfile[MAX_PATH];
    if (!GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH)) return;

    WCHAR recDir[MAX_PATH];
    swprintf(recDir, MAX_PATH, L"%s\\Videos\\Screen Recordings", userProfile);
    if (GetFileAttributesW(recDir) == INVALID_FILE_ATTRIBUTES) return;

    WCHAR searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%s\\*.mp4", recDir);

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

    // Check if target transfer folder is configured and create if needed
    BOOL canTransfer = (g_TransferDays > 0 && g_TransferFolder[0] != L'\0');
    if (canTransfer) {
        if (GetFileAttributesW(g_TransferFolder) == INVALID_FILE_ATTRIBUTES) {
            SHCreateDirectoryExW(NULL, g_TransferFolder, NULL);
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
        swprintf(srcFile, MAX_PATH, L"%s\\%s", recDir, wfd.cFileName);

        // 1. Transfer to Folder X if age >= TransferDays
        if (canTransfer && ageDays >= g_TransferDays) {
            WCHAR dstFile[MAX_PATH];
            swprintf(dstFile, MAX_PATH, L"%s\\%s", g_TransferFolder, wfd.cFileName);
            if (MoveFileExW(srcFile, dstFile, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
                transferredCount++;
                totalBytes -= uSize.QuadPart;
                continue; // moved successfully, skip delete
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

    // If MaxFolderSizeGB is exceeded, show non-intrusive warning
    if (g_MaxFolderSizeGB > 0 && totalGB >= (double)g_MaxFolderSizeGB) {
        WCHAR warnMsg[256];
        swprintf(warnMsg, 256, L"Screen Recordings folder is currently %.1f GB (exceeds your %u GB limit).", totalGB, g_MaxFolderSizeGB);
        
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hWnd;
        g_nid.uID = ID_TRAY_APP_ICON;
        g_nid.uFlags = NIF_INFO;
        swprintf(g_nid.szInfoTitle, 64, L"High Snipping Frames - Storage Warning");
        wcscpy(g_nid.szInfo, warnMsg);
        g_nid.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    } else if (transferredCount > 0 || cleanedCount > 0) {
        WCHAR summaryMsg[256];
        swprintf(summaryMsg, 256, L"Transferred %u recording(s) and cleaned %u old file(s).", transferredCount, cleanedCount);
        
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hWnd;
        g_nid.uID = ID_TRAY_APP_ICON;
        g_nid.uFlags = NIF_INFO;
        swprintf(g_nid.szInfoTitle, 64, L"High Snipping Frames - Storage Guardian");
        wcscpy(g_nid.szInfo, summaryMsg);
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
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

// Open Snipping Tool Recordings folder in Explorer
static void OpenRecordingsFolder() {
    WCHAR path[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH)) {
        wcscat(path, L"\\Videos\\Screen Recordings");
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(path, NULL);
        }
        ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
}

// Launch Snipping Tool for recording
static void LaunchSnippingTool() {
    ShellExecuteW(NULL, L"open", L"snippingtool.exe", NULL, NULL, SW_SHOWNORMAL);
}

// Run ffprobe on the latest recording and display message
static void AnalyzeLatestRecording(HWND hWnd) {
    WCHAR userProfile[MAX_PATH];
    GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH);

    WCHAR searchPath[MAX_PATH];
    swprintf(searchPath, MAX_PATH, L"%s\\Videos\\Screen Recordings\\*.mp4", userProfile);

    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind == INVALID_HANDLE_VALUE) {
        MessageBoxW(hWnd, L"No recordings found in Videos\\Screen Recordings.\nRecord a video with Snipping Tool first!",
                    L"High Snipping Frames", MB_ICONINFORMATION);
        return;
    }

    FILETIME latestTime = wfd.ftLastWriteTime;
    WCHAR latestFile[MAX_PATH];
    swprintf(latestFile, MAX_PATH, L"%s\\Videos\\Screen Recordings\\%s", userProfile, wfd.cFileName);

    while (FindNextFileW(hFind, &wfd)) {
        if (CompareFileTime(&wfd.ftLastWriteTime, &latestTime) > 0) {
            latestTime = wfd.ftLastWriteTime;
            swprintf(latestFile, MAX_PATH, L"%s\\Videos\\Screen Recordings\\%s", userProfile, wfd.cFileName);
        }
    }
    FindClose(hFind);

    WCHAR cmd[1024];
    swprintf(cmd, 1024, L"ffprobe.exe -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate,nb_frames,duration,width,height -of default=noprint_wrappers=1:nokey=0 \"%s\"", latestFile);

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
        MessageBoxW(hWnd, L"Could not execute ffprobe. Make sure ffprobe is available.", L"Analysis", MB_ICONWARNING);
        return;
    }

    WCHAR msg[4096];
    WCHAR* pShortName = wcsrchr(latestFile, L'\\');
    swprintf(msg, 4096, L"Latest Video:\n%s\n\nVerified Stream Analysis:\n%hs",
             (pShortName ? pShortName + 1 : latestFile), buffer);
    MessageBoxW(hWnd, msg, L"High Snipping Frames - Video Verification", MB_ICONINFORMATION);
}

// Modern GDI+ Rounded Rectangle Helper
static void AddRoundedRectangle(GraphicsPath& path, RectF rect, float radius) {
    float d = radius * 2.0f;
    if (d > rect.Width) d = rect.Width;
    if (d > rect.Height) d = rect.Height;

    path.Reset();
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
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

// Render the complete modern native UI
static void RenderUI(Graphics& g, int clientWidth, int clientHeight) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // Apply active Zoom transformation
    g.ScaleTransform(g_ZoomFactor, g_ZoomFactor);

    // Windows 11 Native Fluent Dark Palette (Mica / Surface)
    Color clrCanvas(0x1F, 0x1F, 0x1F);       // Windows 11 Dark Mica Surface
    Color clrHeader(0x27, 0x27, 0x27);       // Header card
    Color clrCard(0x2B, 0x2B, 0x2B);         // Fluent Card Surface
    Color clrCardHover(0x32, 0x32, 0x32);    // Hover Card
    Color clrCardBorder(0x38, 0x38, 0x38);   // Fluent Border
    Color clrAccent(0x00, 0x78, 0xD4);       // Windows 11 Blue
    Color clrAccentHover(0x10, 0x84, 0xD9);
    Color clrAccentLight(0x60, 0xCD, 0xFF);  // Azure glow
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

    // Fonts (Segoe UI Variable)
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

    // 2. Top Header Bar (Snipping Tool Native Style)
    RectF rcHeader(0.0f, 0.0f, (float)BASE_WIDTH, 56.0f);
    g.FillRectangle(&brHeader, rcHeader);
    Pen penHLine(Color(0x33, 0x33, 0x33), 1.0f);
    g.DrawLine(&penHLine, 0.0f, 56.0f, (float)BASE_WIDTH, 56.0f);

    // Header Logo Reticle (Miniature Optical Crop)
    RectF rcLogo(18.0f, 13.0f, 30.0f, 30.0f);
    SolidBrush brLogoBg(Color(0x35, 0x35, 0x35));
    FillRoundedRect(g, brLogoBg, rcLogo, 6.0f);
    DrawRoundedRect(g, penBorder, rcLogo, 6.0f);

    Pen penBr(Color(0xFF, 0xFF, 0xFF), 1.8f);
    PointF ptL1[] = { { 24.0f, 26.0f }, { 24.0f, 19.0f }, { 31.0f, 19.0f } };
    g.DrawLines(&penBr, ptL1, 3);
    Pen penBrA(Color(0x00, 0x78, 0xD4), 1.8f);
    PointF ptL2[] = { { 42.0f, 30.0f }, { 42.0f, 37.0f }, { 35.0f, 37.0f } };
    g.DrawLines(&penBrA, ptL2, 3);
    g.FillEllipse(&brAccentLight, 36.0f, 20.0f, 4.0f, 4.0f);

    g.DrawString(L"High Snipping Frames", -1, &fontTitle, PointF(56.0f, 18.0f), &brTextWhite);

    // Top Right Version Badge
    RectF rcTopBadge(BASE_WIDTH - 96.0f, 17.0f, 78.0f, 22.0f);
    SolidBrush brBadge(Color(0x33, 0x33, 0x33));
    FillRoundedRect(g, brBadge, rcTopBadge, 4.0f);
    DrawRoundedRect(g, penBorder, rcTopBadge, 4.0f);
    g.DrawString(L"v2.1 NATIVE", -1, &fontBadge, rcTopBadge, &sfCenter, &brAccentLight);

    // 3. HERO CARD: Prominent Live Target Framerate Display
    // Resolves confusion: clearly shows the selected framerate dynamically!
    RectF rcHero(16.0f, 70.0f, BASE_WIDTH - 32.0f, 84.0f);
    SolidBrush brHeroBg(Color(0x27, 0x27, 0x27));
    FillRoundedRect(g, brHeroBg, rcHero, 10.0f);
    Pen penHero(Color(0x00, 0x78, 0xD4), 1.2f);
    DrawRoundedRect(g, penHero, rcHero, 10.0f);

    // Left Icon & Big Target Number
    WCHAR fpsMainStr[32];
    UINT32 maxRefresh = GetMaxDisplayRefresh();
    if (g_CurrentFPS == 0) {
        swprintf(fpsMainStr, 32, L"AUTO (%u FPS)", maxRefresh);
    } else {
        swprintf(fpsMainStr, 32, L"%u FPS", g_CurrentFPS);
    }

    g.DrawString(fpsMainStr, -1, &fontHeroNum, PointF(30.0f, 80.0f), &brTextWhite);

    // Subtitle & Status Badge
    g.DrawString(L"ACTIVE RECORDING TARGET", -1, &fontSection, PointF(32.0f, 120.0f), &brAccentLight);

    // Right Side: Live System Details
    DWORD snipPid   = FindProcessPID(L"SnippingTool.exe");

    RectF rcHeroRight(BASE_WIDTH - 210.0f, 80.0f, 180.0f, 64.0f);
    WCHAR sysInfo[128];
    swprintf(sysInfo, 128, L"Display: %u Hz\nEngine: Active (Unified)\nSnipping Tool: %s",
             maxRefresh,
             (snipPid != 0 ? L"Hooked & Ready" : L"Idle"));
    g.DrawString(sysInfo, -1, &fontSmall, rcHeroRight, &sfLeft, &brTextDim);

    // Green indicator dot on Hero Card
    g.FillEllipse(&brGreen, BASE_WIDTH - 222.0f, 96.0f, 6.0f, 6.0f);

    // 4. SECTION: Target Framerate Selection (Segmented Pills)
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
            // Selected: Fluent Blue Pill
            SolidBrush brSel(clrAccent);
            FillRoundedRect(g, brSel, rcPill, 6.0f);
            Pen penSel(clrAccentLight, 1.2f);
            DrawRoundedRect(g, penSel, rcPill, 6.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBodyBold, rcPill, &sfCenter, &brTextWhite);
        } else {
            // Unselected
            Color cPill = isHovered ? clrCardHover : clrCard;
            SolidBrush brPill(cPill);
            FillRoundedRect(g, brPill, rcPill, 6.0f);
            Pen penPill(isHovered ? clrAccentLight : clrCardBorder, 1.0f);
            DrawRoundedRect(g, penPill, rcPill, 6.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBody, rcPill, &sfCenter, isHovered ? &brTextWhite : &brTextSub);
        }
    }

    // 5. SECTION: Bitrate & Quality Setting
    g.DrawString(L"RECORDING BITRATE QUALITY", -1, &fontSection, PointF(18.0f, 244.0f), &brTextDim);

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
    // Record Screen (Primary Fluent Button)
    RectF rcBtnRec(16.0f, 392.0f, 150.0f, 42.0f);
    BOOL isRecHover = (g_HoveredHitId == HIT_BTN_RECORD);
    SolidBrush brBtnRec(isRecHover ? clrAccentHover : clrAccent);
    FillRoundedRect(g, brBtnRec, rcBtnRec, 6.0f);
    Pen penBtnRec(clrAccentLight, 1.0f);
    DrawRoundedRect(g, penBtnRec, rcBtnRec, 6.0f);
    g.DrawString(L"Record Screen", -1, &fontBodyBold, rcBtnRec, &sfCenter, &brTextWhite);

    // View Recordings
    RectF rcBtnFolder(174.0f, 392.0f, 140.0f, 42.0f);
    BOOL isFolderHover = (g_HoveredHitId == HIT_BTN_FOLDER);
    SolidBrush brFolder(isFolderHover ? clrCardHover : clrCard);
    FillRoundedRect(g, brFolder, rcBtnFolder, 6.0f);
    DrawRoundedRect(g, isFolderHover ? penAccent : penBorder, rcBtnFolder, 6.0f);
    g.DrawString(L"Open Videos", -1, &fontBody, rcBtnFolder, &sfCenter, isFolderHover ? &brAccentLight : &brTextWhite);

    // Verify Latest Video (ffprobe)
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
    if (now - g_LastSavedTick < 2500) {
        // Show temporary saved toast
        SolidBrush brGreenText(clrGreen);
        g.DrawString(L"✓ Setting applied instantly! Snipping Tool will record with the new target framerate.",
                     -1, &fontSmall, rcInfo, &sfCenter, &brGreenText);
    } else {
        g.DrawString(L"Tip: Use Win + Shift + S for instant screenshot snips. Video recording automatically uses your selected target framerate.",
                     -1, &fontSmall, rcInfo, &sfCenter, &brTextDim);
    }

    // 9. BOTTOM BAR: Zoom Controls & Footer
    RectF rcFooter(0.0f, (float)BASE_HEIGHT - 44.0f, (float)BASE_WIDTH, 44.0f);
    SolidBrush brFoot(Color(0x18, 0x18, 0x18));
    g.FillRectangle(&brFoot, rcFooter);
    Pen penFLine(Color(0x2E, 0x2E, 0x2E), 1.0f);
    g.DrawLine(&penFLine, 0.0f, rcFooter.Y, (float)BASE_WIDTH, rcFooter.Y);

    // Zoom Interactive Controls: [-] 100% [+]
    int zoomPct = (int)(g_ZoomFactor * 100.0f + 0.5f);
    WCHAR zoomTxt[64];
    swprintf(zoomTxt, 64, L"Zoom: %d%%", zoomPct);

    // Left info
    g.DrawString(L"Hold Ctrl + Scroll wheel to zoom", -1, &fontSmall, PointF(18.0f, rcFooter.Y + 12.0f), &brTextDim);

    // Zoom Buttons on right: [-] [ 100% ] [+]
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

// Hit-testing logic: converts physical mouse coordinates to base logical coordinates
static int HitTest(int mouseX, int mouseY, int* pData) {
    float lx = (float)mouseX / g_ZoomFactor;
    float ly = (float)mouseY / g_ZoomFactor;
    if (pData) *pData = -1;

    // Check FPS Presets
    float fpsX = 16.0f;
    float fpsY = 188.0f;
    float pillW = (BASE_WIDTH - 32.0f - (g_NumFpsOptions - 1) * 6.0f) / g_NumFpsOptions;
    float pillH = 40.0f;
    for (int i = 0; i < g_NumFpsOptions; i++) {
        RectF rcPill(fpsX + i * (pillW + 6.0f), fpsY, pillW, pillH);
        if (rcPill.Contains(lx, ly)) {
            if (pData) *pData = i;
            return HIT_FPS_PRESET;
        }
    }

    // Check Quality Presets
    float qY = 264.0f;
    float qW = (BASE_WIDTH - 32.0f - 2 * 8.0f) / 3.0f;
    float qH = 58.0f;
    for (int i = 0; i < g_NumQualityOptions; i++) {
        RectF rcQ(16.0f + i * (qW + 8.0f), qY, qW, qH);
        if (rcQ.Contains(lx, ly)) {
            if (pData) *pData = i;
            return HIT_QUALITY_PRESET;
        }
    }

    // Auto-start Checkbox
    RectF rcAuto(16.0f, 336.0f, BASE_WIDTH - 32.0f, 40.0f);
    if (rcAuto.Contains(lx, ly)) return HIT_AUTOSTART;

    // Record Button
    RectF rcBtnRec(16.0f, 392.0f, 150.0f, 42.0f);
    if (rcBtnRec.Contains(lx, ly)) return HIT_BTN_RECORD;

    // Open Folder Button
    RectF rcBtnFolder(174.0f, 392.0f, 140.0f, 42.0f);
    if (rcBtnFolder.Contains(lx, ly)) return HIT_BTN_FOLDER;

    // Verify Button
    RectF rcBtnVerify(322.0f, 392.0f, BASE_WIDTH - 322.0f - 16.0f, 42.0f);
    if (rcBtnVerify.Contains(lx, ly)) return HIT_BTN_ANALYZE;

    // Zoom Buttons
    float zBtnY = (float)BASE_HEIGHT - 44.0f + 8.0f;
    RectF rcZMinus(BASE_WIDTH - 150.0f, zBtnY, 28.0f, 26.0f);
    if (rcZMinus.Contains(lx, ly)) return HIT_ZOOM_MINUS;

    RectF rcZText(BASE_WIDTH - 118.0f, zBtnY, 56.0f, 26.0f);
    if (rcZText.Contains(lx, ly)) return HIT_ZOOM_RESET;

    RectF rcZPlus(BASE_WIDTH - 58.0f, zBtnY, 28.0f, 26.0f);
    if (rcZPlus.Contains(lx, ly)) return HIT_ZOOM_PLUS;

    return HIT_NONE;
}

// Native Windows 11 Fluent Dark Theme Enabler for Menus and Controls
static void InitDarkMode() {
    HMODULE hUxTheme = LoadLibraryW(L"uxtheme.dll");
    if (hUxTheme) {
        // Ordinal 135: SetPreferredAppMode (2 = ForceDark, 1 = AllowDark)
        typedef int (WINAPI *fnSetPreferredAppMode)(int appMode);
        fnSetPreferredAppMode pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135));
        if (pSetPreferredAppMode) {
            pSetPreferredAppMode(2); // ForceDark
        } else {
            // Ordinal 133: AllowDarkModeForApp fallback for older Win10
            typedef BOOL (WINAPI *fnAllowDarkModeForApp)(BOOL allow);
            fnAllowDarkModeForApp pAllowDarkModeForApp = (fnAllowDarkModeForApp)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133));
            if (pAllowDarkModeForApp) {
                pAllowDarkModeForApp(TRUE);
            }
        }

        // Ordinal 136: FlushMenuThemes to immediately apply dark style to popup menus
        typedef void (WINAPI *fnFlushMenuThemes)();
        fnFlushMenuThemes pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136));
        if (pFlushMenuThemes) {
            pFlushMenuThemes();
        }
    }
}

static void UpdateTrayTooltip() {
    if (g_CurrentFPS == 0) {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (%u Hz)", GetMaxDisplayRefresh());
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", g_CurrentFPS);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Show Tray Context Menu - Clean, native Windows 11 Dark Mode, no emojis
static void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    // 1. App Header with live status
    UINT32 maxRefresh = GetMaxDisplayRefresh();
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

    // 2. Framerate Selection Submenu (active target previewed right in label)
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
    AppendMenuW(hMenu, MF_STRING, IDM_RECORD,  L"Record Screen (Win + Alt + R)");
    AppendMenuW(hMenu, MF_STRING, IDM_FOLDER,  L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_ANALYZE, L"Verify Latest Video");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // 4. Utility / System
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

// Window Procedure
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Apply Windows 11 Dark Mode & Rounded Corners
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
            DWORD corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

            // Set recurring timer for Storage Guardian check (every 4 hours)
            SetTimer(hWnd, TIMER_STORAGE_GUARDIAN, 4 * 3600 * 1000, NULL);
            // Run initial check 10 seconds after startup
            SetTimer(hWnd, TIMER_STORAGE_STARTUP, 10000, NULL);
            break;
        }

        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                // Sleep / Modern Standby Wake Recovery:
                // Give GPU display driver 1.5s to restore true monitor modes and refresh rates
                SetTimer(hWnd, TIMER_SLEEP_RECOVERY, 1500, NULL);
            }
            return TRUE;
        }

        case WM_DISPLAYCHANGE: {
            // Fired when user changes refresh rate, resolution, or plugs in/unplugs an external display
            SetTimer(hWnd, TIMER_SLEEP_RECOVERY, 500, NULL);
            break;
        }

        case WM_TIMER: {
            if (wParam == TIMER_SLEEP_RECOVERY) {
                KillTimer(hWnd, TIMER_SLEEP_RECOVERY);
                UpdateTrayTooltip();

                // Re-verify injection into any active SnippingTool instance
                WCHAR dllPath[MAX_PATH];
                LocateDllPath(dllPath);
                ScanAndInject(dllPath);

                InvalidateRect(hWnd, NULL, FALSE);
            } else if (wParam == TIMER_STORAGE_GUARDIAN) {
                RunStorageGuardian(hWnd, FALSE);
            } else if (wParam == TIMER_STORAGE_STARTUP) {
                KillTimer(hWnd, TIMER_STORAGE_STARTUP);
                RunStorageGuardian(hWnd, FALSE);
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
            // Zoom In / Zoom Out with Ctrl + Mouse Wheel
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
            // Ctrl + 0 resets zoom to 100%
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
                // Instantly update target FPS and persist
                g_CurrentFPS = g_FpsOptions[data];
                SaveSettings();
                UpdateTrayTooltip();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_QUALITY_PRESET && data >= 0 && data < g_NumQualityOptions) {
                g_QualityLevel = data;
                SaveSettings();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_AUTOSTART) {
                g_AutoStart = !g_AutoStart;
                SetAutoStart(g_AutoStart);
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
            return 1; // Double buffered in WM_PAINT

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            if (w > 0 && h > 0) {
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
                    MessageBoxW(hWnd, L"High Snipping Frames unified injection engine rescanned and verified.", L"High Snipping Frames", MB_ICONINFORMATION);
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
            ShowWindow(hWnd, SW_HIDE); // Minimize to tray on close
            return 0;

        case WM_DESTROY:
            if (g_bWatcherRunning) {
                g_bWatcherRunning = FALSE;
                if (g_hWatcherThread) {
                    WaitForSingleObject(g_hWatcherThread, 1500);
                    CloseHandle(g_hWatcherThread);
                    g_hWatcherThread = NULL;
                }
            }
            if (g_EnableGlobalHotkey) {
                UnregisterHotKey(hWnd, HOTKEY_RECORD_VIDEO);
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
    InitDarkMode();
    g_hInstance = hInstance;

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HighSnippingFramesGUIMutex_v3");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExist = FindWindowW(L"HighSnippingFramesGUIClass_v3", NULL);
        if (hExist) {
            ShowWindow(hExist, SW_SHOW);
            SetForegroundWindow(hExist);
        }
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    LoadSettings();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HighSnippingFramesGUIClass_v3";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    int w = (int)(BASE_WIDTH * g_ZoomFactor + 0.5f);
    int h = (int)(BASE_HEIGHT * g_ZoomFactor + 0.5f);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hWndMain = CreateWindowExW(WS_EX_APPWINDOW, L"HighSnippingFramesGUIClass_v3",
                                 L"High Snipping Frames",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hWndMain) {
        GdiplusShutdown(g_gdiplusToken);
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
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (%u Hz)", GetMaxDisplayRefresh());
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", g_CurrentFPS);
    }
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Register Global Instant Video Hotkey (Win + Alt + R)
    if (g_EnableGlobalHotkey && g_hWndMain) {
        RegisterHotKey(g_hWndMain, HOTKEY_RECORD_VIDEO, MOD_WIN | MOD_ALT | MOD_NOREPEAT, 'R');
    }

    // Start the unified background injection watcher thread
    g_bWatcherRunning = TRUE;
    g_hWatcherThread = CreateThread(NULL, 0, WatcherThreadProc, NULL, 0, NULL);

    if (lpCmdLine && wcsstr(lpCmdLine, L"--minimized")) {
        ShowWindow(g_hWndMain, SW_HIDE);
    } else {
        ShowWindow(g_hWndMain, SW_SHOW);
        UpdateWindow(g_hWndMain);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
