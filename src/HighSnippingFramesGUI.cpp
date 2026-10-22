#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAYICON (WM_USER + 101)
#define ID_TRAY_APP_ICON 1001

// Control IDs
#define ID_COMBO_FPS      2001
#define ID_COMBO_QUALITY  2002
#define ID_CHECK_AUTOSTART 2003
#define ID_BTN_APPLY      2004
#define ID_BTN_RECORD     2005
#define ID_BTN_FOLDER     2006
#define ID_BTN_ANALYZE    2007
#define ID_BTN_MINIMIZE   2008

// Tray Menu IDs
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
#define IDM_EXIT          3024

// DWM Dark Mode attribute definition for older SDKs
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// Global state
static HINSTANCE g_hInstance = NULL;
static HWND g_hWndMain = NULL;
static NOTIFYICONDATAW g_nid;
static WCHAR g_IniPath[MAX_PATH] = { 0 };
static UINT32 g_CurrentFPS = 144;
static int g_QualityLevel = 1; // 0=Balanced, 1=High, 2=Ultra
static HFONT g_hFontTitle = NULL;
static HFONT g_hFontNormal = NULL;
static HFONT g_hFontBold = NULL;
static HBRUSH g_hBrDarkBg = NULL;
static HBRUSH g_hBrCardBg = NULL;

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
        wcscpy(g_IniPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\bin\\SnippingToolHighFPS.ini");
    }
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

// Helper: Detect display refresh rate
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

// Helper: Read Settings from INI
static void LoadSettings() {
    LocateIniPath();
    g_CurrentFPS = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 144, g_IniPath);
    g_QualityLevel = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 1, g_IniPath);
    if (g_QualityLevel > 2) g_QualityLevel = 1;
}

// Helper: Write Settings to INI
static void SaveSettings(UINT32 fps, int quality) {
    g_CurrentFPS = fps;
    g_QualityLevel = quality;

    WCHAR fpsStr[16];
    swprintf(fpsStr, 16, L"%u", fps);
    WritePrivateProfileStringW(L"Settings", L"TargetFPS", fpsStr, g_IniPath);

    WCHAR qualStr[16];
    swprintf(qualStr, 16, L"%d", quality);
    WritePrivateProfileStringW(L"Settings", L"BitrateMultiplier", qualStr, g_IniPath);

    // Update Tray Tooltip
    if (fps == 0) {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - Auto (%u Hz)", GetMaxDisplayRefresh());
    } else {
        swprintf(g_nid.szTip, 128, L"High Snipping Frames - %u FPS", fps);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// Helper: Check AutoStart in HKCU Run
static BOOL IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD len = sizeof(path);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(hKey, L"SnippingToolHighFPSDaemon", NULL, &type, (LPBYTE)path, &len);
        RegCloseKey(hKey);
        return (status == ERROR_SUCCESS);
    }
    return FALSE;
}

// Helper: Set AutoStart in HKCU Run
static void SetAutoStart(BOOL enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR daemonPath[MAX_PATH];
            swprintf(daemonPath, MAX_PATH, L"\"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolDaemon.exe\"");
            RegSetValueExW(hKey, L"SnippingToolHighFPSDaemon", 0, REG_SZ, (const BYTE*)daemonPath, (DWORD)(wcslen(daemonPath) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"SnippingToolHighFPSDaemon");
        }
        RegCloseKey(hKey);
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
        MessageBoxW(hWnd, L"No video recordings found in Videos\\Screen Recordings.\nRecord a video first!",
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

    // Run ffprobe via pipe
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
        MessageBoxW(hWnd, L"Could not execute ffprobe. Make sure ffmpeg/ffprobe is installed.", L"Analysis", MB_ICONWARNING);
        return;
    }

    WCHAR msg[4096];
    WCHAR* pShortName = wcsrchr(latestFile, L'\\');
    swprintf(msg, 4096, L"Latest Video: %s\n\nffprobe Analysis:\n%hs", (pShortName ? pShortName + 1 : latestFile), buffer);
    MessageBoxW(hWnd, msg, L"High Snipping Frames - Video Verification", MB_ICONINFORMATION);
}

// Update UI Controls with current state
static void UpdateUI(HWND hWnd) {
    HWND hFps = GetDlgItem(hWnd, ID_COMBO_FPS);
    HWND hQual = GetDlgItem(hWnd, ID_COMBO_QUALITY);
    HWND hAuto = GetDlgItem(hWnd, ID_CHECK_AUTOSTART);

    // Select FPS index
    int sel = 0;
    if (g_CurrentFPS == 60)  sel = 1;
    else if (g_CurrentFPS == 90)  sel = 2;
    else if (g_CurrentFPS == 120) sel = 3;
    else if (g_CurrentFPS == 144) sel = 4;
    else if (g_CurrentFPS == 165) sel = 5;
    else if (g_CurrentFPS == 240) sel = 6;
    SendMessageW(hFps, CB_SETCURSEL, sel, 0);

    SendMessageW(hQual, CB_SETCURSEL, g_QualityLevel, 0);
    SendMessageW(hAuto, BM_SETCHECK, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    InvalidateRect(hWnd, NULL, TRUE);
}

// Show Tray Context Menu
static void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"High Snipping Frames Control Center");
    SetMenuDefaultItem(hMenu, IDM_OPEN, FALSE);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    // FPS Submenu
    HMENU hFpsMenu = CreatePopupMenu();
    UINT autoCheck = (g_CurrentFPS == 0) ? MF_CHECKED : MF_UNCHECKED;
    AppendMenuW(hFpsMenu, MF_STRING | autoCheck, IDM_FPS_AUTO, L"Auto (Match Display)");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 60) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_60, L"60 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 90) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_90, L"90 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 120) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_120, L"120 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 144) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_144, L"144 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 165) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_165, L"165 FPS");
    AppendMenuW(hFpsMenu, MF_STRING | ((g_CurrentFPS == 240) ? MF_CHECKED : MF_UNCHECKED), IDM_FPS_240, L"240 FPS");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFpsMenu, L"FPS Target");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_RECORD, L"Record Screen (Snipping Tool)");
    AppendMenuW(hMenu, MF_STRING, IDM_FOLDER, L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_ANALYZE, L"Analyze Latest Video");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_RESTART_SVC, L"Restart Daemon Service");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
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

            // Fonts
            g_hFontTitle = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Display");
            g_hFontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Text");
            g_hFontBold = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Text");

            g_hBrDarkBg = CreateSolidBrush(RGB(18, 20, 26));
            g_hBrCardBg = CreateSolidBrush(RGB(26, 30, 40));

            // Controls
            HWND hFps = CreateWindowExW(0, L"COMBOBOX", NULL,
                                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                        150, 150, 160, 220, hWnd, (HMENU)ID_COMBO_FPS, g_hInstance, NULL);
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"Auto (Match Display)");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"90 FPS");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"120 FPS");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"144 FPS (Recommended)");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"165 FPS");
            SendMessageW(hFps, CB_ADDSTRING, 0, (LPARAM)L"240 FPS");
            SendMessageW(hFps, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND hQual = CreateWindowExW(0, L"COMBOBOX", NULL,
                                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                         150, 195, 160, 120, hWnd, (HMENU)ID_COMBO_QUALITY, g_hInstance, NULL);
            SendMessageW(hQual, CB_ADDSTRING, 0, (LPARAM)L"Balanced (1.0x Bitrate)");
            SendMessageW(hQual, CB_ADDSTRING, 0, (LPARAM)L"High Quality (1.5x)");
            SendMessageW(hQual, CB_ADDSTRING, 0, (LPARAM)L"Ultra / Esports (2.0x)");
            SendMessageW(hQual, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND hAuto = CreateWindowExW(0, L"BUTTON", L"Start High Snipping Frames with Windows",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         30, 240, 320, 24, hWnd, (HMENU)ID_CHECK_AUTOSTART, g_hInstance, NULL);
            SendMessageW(hAuto, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            // Action Buttons
            HWND btnApply = CreateWindowExW(0, L"BUTTON", L"Apply Settings",
                                            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                            30, 280, 140, 34, hWnd, (HMENU)ID_BTN_APPLY, g_hInstance, NULL);
            SendMessageW(btnApply, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

            HWND btnRecord = CreateWindowExW(0, L"BUTTON", L"Record Screen",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             180, 280, 130, 34, hWnd, (HMENU)ID_BTN_RECORD, g_hInstance, NULL);
            SendMessageW(btnRecord, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND btnAnalyze = CreateWindowExW(0, L"BUTTON", L"Analyze Latest Video",
                                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              30, 324, 140, 32, hWnd, (HMENU)ID_BTN_ANALYZE, g_hInstance, NULL);
            SendMessageW(btnAnalyze, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND btnFolder = CreateWindowExW(0, L"BUTTON", L"Open Videos",
                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             180, 324, 130, 32, hWnd, (HMENU)ID_BTN_FOLDER, g_hInstance, NULL);
            SendMessageW(btnFolder, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND btnMin = CreateWindowExW(0, L"BUTTON", L"Minimize to Tray",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          320, 324, 120, 32, hWnd, (HMENU)ID_BTN_MINIMIZE, g_hInstance, NULL);
            SendMessageW(btnMin, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            UpdateUI(hWnd);
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            FillRect(hdc, &rcClient, g_hBrDarkBg);

            // Header Background
            RECT rcHeader = { 0, 0, rcClient.right, 70 };
            FillRect(hdc, &rcHeader, g_hBrCardBg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(245, 247, 250));
            SelectObject(hdc, g_hFontTitle);
            TextOutW(hdc, 24, 12, L"High Snipping Frames", 20);

            SetTextColor(hdc, RGB(56, 189, 248)); // Accent
            SelectObject(hdc, g_hFontNormal);
            TextOutW(hdc, 24, 40, L"Native High-FPS Recording for Windows Snipping Tool", 51);

            // Status Card
            RECT rcCard = { 20, 80, rcClient.right - 20, 135 };
            FillRect(hdc, &rcCard, g_hBrCardBg);
            FrameRect(hdc, &rcCard, (HBRUSH)GetStockObject(BLACK_BRUSH));

            DWORD daemonPid = FindProcessPID(L"SnippingToolDaemon.exe");
            DWORD snipPid = FindProcessPID(L"SnippingTool.exe");
            UINT32 maxRefresh = GetMaxDisplayRefresh();

            SetTextColor(hdc, RGB(220, 225, 235));
            SelectObject(hdc, g_hFontBold);
            TextOutW(hdc, 32, 90, L"Service Status:", 15);

            SelectObject(hdc, g_hFontNormal);
            if (daemonPid != 0) {
                SetTextColor(hdc, RGB(52, 211, 153)); // Green
                WCHAR dStr[64];
                swprintf(dStr, 64, L"Daemon Active (PID %u)", daemonPid);
                TextOutW(hdc, 150, 90, dStr, (int)wcslen(dStr));
            } else {
                SetTextColor(hdc, RGB(248, 113, 113)); // Red
                TextOutW(hdc, 150, 90, L"Daemon Inactive", 15);
            }

            SetTextColor(hdc, RGB(220, 225, 235));
            TextOutW(hdc, 32, 110, L"Snipping Tool:", 14);
            if (snipPid != 0) {
                SetTextColor(hdc, RGB(52, 211, 153));
                WCHAR sStr[64];
                swprintf(sStr, 64, L"Running (PID %u - High FPS Active)", snipPid);
                TextOutW(hdc, 150, 110, sStr, (int)wcslen(sStr));
            } else {
                SetTextColor(hdc, RGB(148, 163, 184));
                TextOutW(hdc, 150, 110, L"Idle (Will auto-hook upon launch)", 32);
            }

            // Labels
            SetTextColor(hdc, RGB(220, 225, 235));
            SelectObject(hdc, g_hFontBold);
            TextOutW(hdc, 30, 152, L"Target Framerate:", 17);
            TextOutW(hdc, 30, 197, L"Bitrate Quality:", 16);

            // Display Info
            WCHAR dispInfo[64];
            swprintf(dispInfo, 64, L"Display Refresh: %u Hz", maxRefresh);
            SetTextColor(hdc, RGB(148, 163, 184));
            SelectObject(hdc, g_hFontNormal);
            TextOutW(hdc, 320, 152, dispInfo, (int)wcslen(dispInfo));

            EndPaint(hWnd, &ps);
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case ID_BTN_APPLY: {
                    HWND hFps = GetDlgItem(hWnd, ID_COMBO_FPS);
                    int selFps = (int)SendMessageW(hFps, CB_GETCURSEL, 0, 0);
                    UINT32 fpsList[] = { 0, 60, 90, 120, 144, 165, 240 };
                    UINT32 chosenFps = (selFps >= 0 && selFps < 7) ? fpsList[selFps] : 144;

                    HWND hQual = GetDlgItem(hWnd, ID_COMBO_QUALITY);
                    int chosenQual = (int)SendMessageW(hQual, CB_GETCURSEL, 0, 0);
                    if (chosenQual < 0) chosenQual = 1;

                    HWND hAuto = GetDlgItem(hWnd, ID_CHECK_AUTOSTART);
                    BOOL autoStart = (SendMessageW(hAuto, BM_GETCHECK, 0, 0) == BST_CHECKED);

                    SaveSettings(chosenFps, chosenQual);
                    SetAutoStart(autoStart);

                    MessageBoxW(hWnd, L"Settings saved successfully!\nSnipping Tool will record with the new framerate immediately.",
                                L"High Snipping Frames", MB_ICONINFORMATION);
                    break;
                }

                case ID_BTN_RECORD:
                case IDM_RECORD:
                    LaunchSnippingTool();
                    break;

                case ID_BTN_FOLDER:
                case IDM_FOLDER:
                    OpenRecordingsFolder();
                    break;

                case ID_BTN_ANALYZE:
                case IDM_ANALYZE:
                    AnalyzeLatestRecording(hWnd);
                    break;

                case ID_BTN_MINIMIZE:
                    ShowWindow(hWnd, SW_HIDE);
                    break;

                case IDM_OPEN:
                    ShowWindow(hWnd, SW_SHOW);
                    SetForegroundWindow(hWnd);
                    break;

                case IDM_FPS_AUTO: SaveSettings(0, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_60:   SaveSettings(60, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_90:   SaveSettings(90, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_120:  SaveSettings(120, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_144:  SaveSettings(144, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_165:  SaveSettings(165, g_QualityLevel); UpdateUI(hWnd); break;
                case IDM_FPS_240:  SaveSettings(240, g_QualityLevel); UpdateUI(hWnd); break;

                case IDM_RESTART_SVC: {
                    StopProcess:
                    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (hSnap != INVALID_HANDLE_VALUE) {
                        PROCESSENTRY32W pe = { sizeof(pe) };
                        if (Process32FirstW(hSnap, &pe)) {
                            do {
                                if (_wcsicmp(pe.szExeFile, L"SnippingToolDaemon.exe") == 0) {
                                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                                    if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
                                }
                            } while (Process32NextW(hSnap, &pe));
                        }
                        CloseHandle(hSnap);
                    }
                    Sleep(300);
                    ShellExecuteW(NULL, L"open", L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolDaemon.exe", NULL, NULL, SW_HIDE);
                    MessageBoxW(hWnd, L"High Snipping Frames background daemon restarted.", L"Service", MB_ICONINFORMATION);
                    InvalidateRect(hWnd, NULL, TRUE);
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
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hFontTitle) DeleteObject(g_hFontTitle);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hBrDarkBg) DeleteObject(g_hBrDarkBg);
            if (g_hBrCardBg) DeleteObject(g_hBrCardBg);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    // Single instance mutex
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HighSnippingFramesGUIMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExist = FindWindowW(L"HighSnippingFramesGUIClass", NULL);
        if (hExist) {
            ShowWindow(hExist, SW_SHOW);
            SetForegroundWindow(hExist);
        }
        return 0;
    }

    LoadSettings();

    // Register Window Class
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HighSnippingFramesGUIClass";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    // Create Main Window centered
    int w = 480;
    int h = 410;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    g_hWndMain = CreateWindowExW(WS_EX_APPWINDOW, L"HighSnippingFramesGUIClass",
                                 L"High Snipping Frames - Control Center",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hWndMain) return 0;

    // Initialize System Tray Icon
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

    // Show window on launch unless --minimized passed
    if (lpCmdLine && wcsstr(lpCmdLine, L"--minimized")) {
        ShowWindow(g_hWndMain, SW_HIDE);
    } else {
        ShowWindow(g_hWndMain, SW_SHOW);
        UpdateWindow(g_hWndMain);
    }

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    return 0;
}
