#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
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
#define IDM_EXIT          3025

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// Base logical design resolution (100% zoom)
const int BASE_WIDTH  = 540;
const int BASE_HEIGHT = 580;

// Global application state
static HINSTANCE g_hInstance = NULL;
static HWND g_hWndMain = NULL;
static NOTIFYICONDATAW g_nid;
static ULONG_PTR g_gdiplusToken = 0;
static WCHAR g_IniPath[MAX_PATH] = { 0 };

static UINT32 g_CurrentFPS = 144;
static int    g_QualityLevel = 1; // 0=Balanced, 1=High, 2=Ultra
static float  g_ZoomFactor = 1.0f; // Zoom: 0.75f - 2.5f
static BOOL   g_AutoStart = TRUE;

// Hit test areas (in base logical coordinates)
struct ClickableRect {
    RectF rc;
    int   id;
    int   data;
};

#define HIT_NONE           0
#define HIT_FPS_PRESET     1
#define HIT_QUALITY_PRESET 2
#define HIT_AUTOSTART      3
#define HIT_BTN_RECORD     4
#define HIT_BTN_FOLDER     5
#define HIT_BTN_ANALYZE    6
#define HIT_BTN_MINIMIZE   7
#define HIT_BTN_APPLY      8

static int g_HoveredHitId = HIT_NONE;
static int g_HoveredData = -1;

// FPS presets: 0 = Auto, followed by fixed rates
static const UINT32 g_FpsOptions[] = { 0, 60, 90, 120, 144, 165, 240 };
static const WCHAR* g_FpsLabels[]  = { L"AUTO", L"60", L"90", L"120", L"144", L"165", L"240" };
static const int g_NumFpsOptions   = 7;

// Quality presets
static const WCHAR* g_QualityLabels[] = { L"Balanced (1.0x)", L"High Quality (1.5x)", L"Ultra Gaming (2.0x)" };
static const WCHAR* g_QualityDescs[]  = { L"Standard bitrates", L"Crisp motion", L"Zero compression artifacts" };
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
        wcscpy(g_IniPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\bin\\SnippingToolHighFPS.ini");
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

            WCHAR guiPath[MAX_PATH];
            swprintf(guiPath, MAX_PATH, L"\"C:\\Program Files\\SnippingToolHighFPS\\HighSnippingFramesGUI.exe\" --minimized");
            RegSetValueExW(hKey, L"HighSnippingFramesGUI", 0, REG_SZ, (const BYTE*)guiPath, (DWORD)(wcslen(guiPath) + 1) * sizeof(WCHAR));
        } else {
            RegDeleteValueW(hKey, L"SnippingToolHighFPSDaemon");
            RegDeleteValueW(hKey, L"HighSnippingFramesGUI");
        }
        RegCloseKey(hKey);
    }
}

// Helper: Read Settings from INI
static void LoadSettings() {
    LocateIniPath();
    g_CurrentFPS = GetPrivateProfileIntW(L"Settings", L"TargetFPS", 144, g_IniPath);
    g_QualityLevel = GetPrivateProfileIntW(L"Settings", L"BitrateMultiplier", 1, g_IniPath);
    if (g_QualityLevel > 2) g_QualityLevel = 1;

    // Load Zoom Factor (defaults to 1.0)
    int zoomPercent = GetPrivateProfileIntW(L"GUI", L"ZoomPercent", 100, g_IniPath);
    if (zoomPercent >= 75 && zoomPercent <= 250) {
        g_ZoomFactor = (float)zoomPercent / 100.0f;
    }
    g_AutoStart = IsAutoStartEnabled();
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
        MessageBoxW(hWnd, L"No video recordings found in Videos\\Screen Recordings.\nRecord a video with Snipping Tool first!",
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
        MessageBoxW(hWnd, L"Could not execute ffprobe. Make sure ffprobe is available in PATH.", L"Analysis", MB_ICONWARNING);
        return;
    }

    WCHAR msg[4096];
    WCHAR* pShortName = wcsrchr(latestFile, L'\\');
    swprintf(msg, 4096, L"Latest Video File:\n%s\n\nVerified Stream Analysis:\n%hs",
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

    // Colors
    Color clrBg(0x0A, 0x0D, 0x14);        // Canvas background
    Color clrHeader(0x0E, 0x12, 0x1D);    // Top Header background
    Color clrCard(0x13, 0x17, 0x24);      // Card background
    Color clrCardBorder(0x22, 0x2A, 0x3D); // Card border
    Color clrTextWhite(0xF8, 0xFA, 0xFC);
    Color clrTextDim(0x8C, 0x96, 0xAA);
    Color clrTextMuted(0x56, 0x60, 0x76);
    Color clrAccent(0x00, 0x78, 0xD4);    // Windows Azure
    Color clrAccentLight(0x38, 0xBD, 0xF8);
    Color clrGreen(0x34, 0xD3, 0x99);

    // Brushes
    SolidBrush brBg(clrBg);
    SolidBrush brHeader(clrHeader);
    SolidBrush brCard(clrCard);
    SolidBrush brAccent(clrAccent);
    SolidBrush brAccentLight(clrAccentLight);
    SolidBrush brTextWhite(clrTextWhite);
    SolidBrush brTextDim(clrTextDim);
    SolidBrush brTextMuted(clrTextMuted);
    SolidBrush brGreen(clrGreen);

    // Pens
    Pen penCardBorder(clrCardBorder, 1.0f);
    Pen penAccent(clrAccentLight, 1.5f);

    // Fonts
    FontFamily ff(L"Segoe UI");
    Font fontTitle(&ff, 17, FontStyleBold, UnitPixel);
    Font fontSubtitle(&ff, 11, FontStyleRegular, UnitPixel);
    Font fontSection(&ff, 11, FontStyleBold, UnitPixel);
    Font fontBody(&ff, 12, FontStyleRegular, UnitPixel);
    Font fontBodyBold(&ff, 12, FontStyleBold, UnitPixel);
    Font fontSmall(&ff, 10, FontStyleRegular, UnitPixel);
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

    // 1. Draw Canvas Background
    g.FillRectangle(&brBg, 0.0f, 0.0f, (float)BASE_WIDTH, (float)BASE_HEIGHT);

    // 2. Header Panel
    RectF rcHeader(0.0f, 0.0f, (float)BASE_WIDTH, 74.0f);
    g.FillRectangle(&brHeader, rcHeader);
    Pen penHeaderLine(Color(0x1E, 0x24, 0x36), 1.0f);
    g.DrawLine(&penHeaderLine, 0.0f, 74.0f, (float)BASE_WIDTH, 74.0f);

    // Render Logo / Icon in Header
    // Icon squircle badge at (20, 16) 42x42
    RectF rcLogo(22.0f, 16.0f, 42.0f, 42.0f);
    SolidBrush brLogoBg(Color(0x15, 0x19, 0x26));
    FillRoundedRect(g, brLogoBg, rcLogo, 10.0f);
    Pen penLogoBorder(Color(0x38, 0xBD, 0xF8), 1.2f);
    DrawRoundedRect(g, penLogoBorder, rcLogo, 10.0f);

    // Logo internal geometry (Miniature Optical Crop Reticle)
    Pen penBracket(Color(0xFF, 0xFF, 0xFF), 2.2f);
    // Top-left bracket
    PointF ptTL[] = { { 30.0f, 32.0f }, { 30.0f, 24.0f }, { 38.0f, 24.0f } };
    g.DrawLines(&penBracket, ptTL, 3);
    // Bottom-right bracket
    Pen penAccentBracket(Color(0x00, 0x78, 0xD4), 2.2f);
    PointF ptBR[] = { { 56.0f, 42.0f }, { 56.0f, 50.0f }, { 48.0f, 50.0f } };
    g.DrawLines(&penAccentBracket, ptBR, 3);
    // Center lens optical dot
    g.FillEllipse(&brAccentLight, 48.0f, 26.0f, 6.0f, 6.0f);
    g.FillEllipse(&brTextWhite, 32.0f, 42.0f, 5.0f, 5.0f);

    // Header Title and Subtitle
    g.DrawString(L"High Snipping Frames", -1, &fontTitle, PointF(76.0f, 18.0f), &brTextWhite);
    g.DrawString(L"Native High-FPS Recording for Windows Snipping Tool", -1, &fontSubtitle, PointF(76.0f, 42.0f), &brAccentLight);

    // Top Right Version & Native Badge
    RectF rcBadge(BASE_WIDTH - 110.0f, 26.0f, 88.0f, 22.0f);
    SolidBrush brBadgeBg(Color(0x18, 0x2A, 0x40));
    FillRoundedRect(g, brBadgeBg, rcBadge, 6.0f);
    Pen penBadgeBorder(Color(0x00, 0x78, 0xD4), 1.0f);
    DrawRoundedRect(g, penBadgeBorder, rcBadge, 6.0f);
    g.DrawString(L"v2.0 • NATIVE", -1, &fontBadge, rcBadge, &sfCenter, &brAccentLight);

    // 3. Status Overview Card
    RectF rcStatus(20.0f, 88.0f, BASE_WIDTH - 40.0f, 68.0f);
    FillRoundedRect(g, brCard, rcStatus, 12.0f);
    DrawRoundedRect(g, penCardBorder, rcStatus, 12.0f);

    DWORD daemonPid = FindProcessPID(L"SnippingToolDaemon.exe");
    DWORD snipPid   = FindProcessPID(L"SnippingTool.exe");
    UINT32 maxRefresh = GetMaxDisplayRefresh();

    // Pulse dot (green if active)
    g.FillEllipse(&brGreen, 36.0f, 102.0f, 8.0f, 8.0f);
    WCHAR dStatus[64];
    if (daemonPid != 0) {
        swprintf(dStatus, 64, L"High-FPS Daemon: Active (PID %u)", daemonPid);
        g.DrawString(dStatus, -1, &fontBodyBold, PointF(52.0f, 98.0f), &brGreen);
    } else {
        SolidBrush brRed(Color(0xF8, 0x71, 0x71));
        g.DrawString(L"High-FPS Daemon: Inactive", -1, &fontBodyBold, PointF(52.0f, 98.0f), &brRed);
    }

    // Display info & Snipping tool status line
    WCHAR subStatus[128];
    swprintf(subStatus, 128, L"Display Refresh: %u Hz  •  Snipping Tool: %s",
             maxRefresh, (snipPid != 0 ? L"Active (Hooked)" : L"Ready to record"));
    g.DrawString(subStatus, -1, &fontSubtitle, PointF(36.0f, 126.0f), &brTextDim);

    // 4. Section: Target Framerate Selection
    g.DrawString(L"TARGET FRAMERATE", -1, &fontSection, PointF(22.0f, 172.0f), &brTextMuted);

    // Render 7 Interactive Segmented FPS Pills
    float fpsCardX = 20.0f;
    float fpsCardY = 192.0f;
    float pillW = (BASE_WIDTH - 40.0f - (g_NumFpsOptions - 1) * 6.0f) / g_NumFpsOptions;
    float pillH = 44.0f;

    for (int i = 0; i < g_NumFpsOptions; i++) {
        RectF rcPill(fpsCardX + i * (pillW + 6.0f), fpsCardY, pillW, pillH);
        BOOL isSelected = (g_CurrentFPS == g_FpsOptions[i]);
        BOOL isHovered  = (g_HoveredHitId == HIT_FPS_PRESET && g_HoveredData == i);

        if (isSelected) {
            // Active selection: Azure glowing gradient
            LinearGradientBrush brSel(rcPill, Color(0x00, 0x78, 0xD4), Color(0x02, 0x84, 0xC7), LinearGradientModeVertical);
            FillRoundedRect(g, brSel, rcPill, 8.0f);
            Pen penSel(Color(0x38, 0xBD, 0xF8), 1.5f);
            DrawRoundedRect(g, penSel, rcPill, 8.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBodyBold, rcPill, &sfCenter, &brTextWhite);
        } else {
            // Inactive
            Color cPillBg = isHovered ? Color(0x1C, 0x22, 0x34) : Color(0x13, 0x17, 0x24);
            SolidBrush brInact(cPillBg);
            FillRoundedRect(g, brInact, rcPill, 8.0f);
            Pen penInact(isHovered ? Color(0x38, 0xBD, 0xF8) : clrCardBorder, 1.0f);
            DrawRoundedRect(g, penInact, rcPill, 8.0f);
            g.DrawString(g_FpsLabels[i], -1, &fontBody, rcPill, &sfCenter, isHovered ? &brTextWhite : &brTextDim);
        }
    }

    // 5. Section: Bitrate Quality Presets
    g.DrawString(L"RECORDING BITRATE QUALITY", -1, &fontSection, PointF(22.0f, 252.0f), &brTextMuted);

    float qCardY = 272.0f;
    float qCardW = (BASE_WIDTH - 40.0f - 2 * 8.0f) / 3.0f;
    float qCardH = 64.0f;

    for (int i = 0; i < g_NumQualityOptions; i++) {
        RectF rcQ(20.0f + i * (qCardW + 8.0f), qCardY, qCardW, qCardH);
        BOOL isSelected = (g_QualityLevel == i);
        BOOL isHovered  = (g_HoveredHitId == HIT_QUALITY_PRESET && g_HoveredData == i);

        Color cBg = isSelected ? Color(0x10, 0x24, 0x3E) : (isHovered ? Color(0x1C, 0x22, 0x34) : Color(0x13, 0x17, 0x24));
        SolidBrush brQ(cBg);
        FillRoundedRect(g, brQ, rcQ, 10.0f);
        Pen penQ(isSelected ? Color(0x38, 0xBD, 0xF8) : (isHovered ? Color(0x2E, 0x38, 0x50) : clrCardBorder), isSelected ? 1.5f : 1.0f);
        DrawRoundedRect(g, penQ, rcQ, 10.0f);

        RectF rcQTitle(rcQ.X + 10.0f, rcQ.Y + 10.0f, rcQ.Width - 20.0f, 20.0f);
        RectF rcQDesc(rcQ.X + 10.0f, rcQ.Y + 34.0f, rcQ.Width - 20.0f, 20.0f);
        g.DrawString(g_QualityLabels[i], -1, &fontBodyBold, rcQTitle, &sfLeft, isSelected ? &brAccentLight : &brTextWhite);
        g.DrawString(g_QualityDescs[i], -1, &fontSmall, rcQDesc, &sfLeft, &brTextDim);
    }

    // 6. Auto-start Checkbox Card
    RectF rcAuto(20.0f, 350.0f, BASE_WIDTH - 40.0f, 44.0f);
    BOOL isAutoHovered = (g_HoveredHitId == HIT_AUTOSTART);
    SolidBrush brAutoBg(isAutoHovered ? Color(0x17, 0x1C, 0x2B) : clrCard);
    FillRoundedRect(g, brAutoBg, rcAuto, 8.0f);
    DrawRoundedRect(g, penCardBorder, rcAuto, 8.0f);

    // Checkbox square
    RectF rcBox(34.0f, 363.0f, 18.0f, 18.0f);
    if (g_AutoStart) {
        FillRoundedRect(g, brAccent, rcBox, 4.0f);
        // Checkmark
        Pen penCheck(Color(0xFF, 0xFF, 0xFF), 2.0f);
        PointF ptC[] = { { 38.0f, 372.0f }, { 42.0f, 376.0f }, { 48.0f, 368.0f } };
        g.DrawLines(&penCheck, ptC, 3);
    } else {
        Pen penBox(clrTextDim, 1.5f);
        DrawRoundedRect(g, penBox, rcBox, 4.0f);
    }
    g.DrawString(L"Start High Snipping Frames automatically with Windows", -1, &fontBody, PointF(62.0f, 361.0f), &brTextWhite);

    // 7. Action Button Bar
    // Button: Apply Settings (Prominent)
    RectF rcBtnApply(20.0f, 410.0f, 140.0f, 42.0f);
    BOOL isApplyHover = (g_HoveredHitId == HIT_BTN_APPLY);
    LinearGradientBrush brApply(rcBtnApply,
                                isApplyHover ? Color(0x02, 0x84, 0xC7) : Color(0x00, 0x78, 0xD4),
                                isApplyHover ? Color(0x00, 0x78, 0xD4) : Color(0x00, 0x5A, 0x9E),
                                LinearGradientModeVertical);
    FillRoundedRect(g, brApply, rcBtnApply, 8.0f);
    Pen penApply(Color(0x38, 0xBD, 0xF8), 1.0f);
    DrawRoundedRect(g, penApply, rcBtnApply, 8.0f);
    g.DrawString(L"Apply Settings", -1, &fontBodyBold, rcBtnApply, &sfCenter, &brTextWhite);

    // Button: Start Recording (Snipping Tool)
    RectF rcBtnRec(168.0f, 410.0f, 150.0f, 42.0f);
    BOOL isRecHover = (g_HoveredHitId == HIT_BTN_RECORD);
    SolidBrush brRec(isRecHover ? Color(0x22, 0x2A, 0x3E) : Color(0x15, 0x1A, 0x28));
    FillRoundedRect(g, brRec, rcBtnRec, 8.0f);
    Pen penRec(isRecHover ? Color(0x38, 0xBD, 0xF8) : clrCardBorder, 1.0f);
    DrawRoundedRect(g, penRec, rcBtnRec, 8.0f);
    g.DrawString(L"Record Screen", -1, &fontBody, rcBtnRec, &sfCenter, isRecHover ? &brAccentLight : &brTextWhite);

    // Button: Analyze Latest Recording
    RectF rcBtnAnalyze(326.0f, 410.0f, 194.0f, 42.0f);
    BOOL isAnalyzeHover = (g_HoveredHitId == HIT_BTN_ANALYZE);
    SolidBrush brAnalyze(isAnalyzeHover ? Color(0x22, 0x2A, 0x3E) : Color(0x15, 0x1A, 0x28));
    FillRoundedRect(g, brAnalyze, rcBtnAnalyze, 8.0f);
    Pen penAnalyze(isAnalyzeHover ? Color(0x38, 0xBD, 0xF8) : clrCardBorder, 1.0f);
    DrawRoundedRect(g, penAnalyze, rcBtnAnalyze, 8.0f);
    g.DrawString(L"Analyze Latest Video", -1, &fontBody, rcBtnAnalyze, &sfCenter, isAnalyzeHover ? &brAccentLight : &brTextWhite);

    // 8. Secondary Buttons: Open Folder & Minimize
    RectF rcBtnFolder(20.0f, 460.0f, 150.0f, 36.0f);
    BOOL isFolderHover = (g_HoveredHitId == HIT_BTN_FOLDER);
    SolidBrush brFolder(isFolderHover ? Color(0x1C, 0x22, 0x34) : Color(0x11, 0x15, 0x20));
    FillRoundedRect(g, brFolder, rcBtnFolder, 8.0f);
    Pen penFolder(isFolderHover ? Color(0x38, 0xBD, 0xF8) : clrCardBorder, 1.0f);
    DrawRoundedRect(g, penFolder, rcBtnFolder, 8.0f);
    g.DrawString(L"Open Recordings", -1, &fontSubtitle, rcBtnFolder, &sfCenter, isFolderHover ? &brAccentLight : &brTextDim);

    RectF rcBtnMin(178.0f, 460.0f, 140.0f, 36.0f);
    BOOL isMinHover = (g_HoveredHitId == HIT_BTN_MINIMIZE);
    SolidBrush brMin(isMinHover ? Color(0x1C, 0x22, 0x34) : Color(0x11, 0x15, 0x20));
    FillRoundedRect(g, brMin, rcBtnMin, 8.0f);
    Pen penMin(isMinHover ? Color(0x38, 0xBD, 0xF8) : clrCardBorder, 1.0f);
    DrawRoundedRect(g, penMin, rcBtnMin, 8.0f);
    g.DrawString(L"Minimize to Tray", -1, &fontSubtitle, rcBtnMin, &sfCenter, isMinHover ? &brAccentLight : &brTextDim);

    // 9. Modern Bottom Status & Zoom Indicator
    RectF rcFooter(0.0f, (float)BASE_HEIGHT - 38.0f, (float)BASE_WIDTH, 38.0f);
    SolidBrush brFooter(Color(0x09, 0x0C, 0x12));
    g.FillRectangle(&brFooter, rcFooter);
    Pen penFooterLine(Color(0x1A, 0x20, 0x30), 1.0f);
    g.DrawLine(&penFooterLine, 0.0f, rcFooter.Y, (float)BASE_WIDTH, rcFooter.Y);

    WCHAR zoomInfo[128];
    int curZoomPct = (int)(g_ZoomFactor * 100.0f + 0.5f);
    swprintf(zoomInfo, 128, L"Zoom: %d%%  (Ctrl + Scroll to zoom • Ctrl + 0 to reset)", curZoomPct);
    RectF rcZoomText(20.0f, rcFooter.Y, BASE_WIDTH - 40.0f, 38.0f);
    g.DrawString(zoomInfo, -1, &fontSmall, rcZoomText, &sfLeft, &brTextDim);

    WCHAR verText[] = L"Windows 11 24H2 Ready";
    g.DrawString(verText, -1, &fontSmall, rcZoomText, &sfRight, &brAccentLight);
}

// Hit-testing logic: converts physical mouse coordinates to base logical coordinates
static int HitTest(int mouseX, int mouseY, int* pData) {
    float lx = (float)mouseX / g_ZoomFactor;
    float ly = (float)mouseY / g_ZoomFactor;
    if (pData) *pData = -1;

    // Check FPS Presets
    float fpsCardX = 20.0f;
    float fpsCardY = 192.0f;
    float pillW = (BASE_WIDTH - 40.0f - (g_NumFpsOptions - 1) * 6.0f) / g_NumFpsOptions;
    float pillH = 44.0f;
    for (int i = 0; i < g_NumFpsOptions; i++) {
        RectF rcPill(fpsCardX + i * (pillW + 6.0f), fpsCardY, pillW, pillH);
        if (rcPill.Contains(lx, ly)) {
            if (pData) *pData = i;
            return HIT_FPS_PRESET;
        }
    }

    // Check Quality Presets
    float qCardY = 272.0f;
    float qCardW = (BASE_WIDTH - 40.0f - 2 * 8.0f) / 3.0f;
    float qCardH = 64.0f;
    for (int i = 0; i < g_NumQualityOptions; i++) {
        RectF rcQ(20.0f + i * (qCardW + 8.0f), qCardY, qCardW, qCardH);
        if (rcQ.Contains(lx, ly)) {
            if (pData) *pData = i;
            return HIT_QUALITY_PRESET;
        }
    }

    // Auto-start Checkbox
    RectF rcAuto(20.0f, 350.0f, BASE_WIDTH - 40.0f, 44.0f);
    if (rcAuto.Contains(lx, ly)) return HIT_AUTOSTART;

    // Apply Button
    RectF rcBtnApply(20.0f, 410.0f, 140.0f, 42.0f);
    if (rcBtnApply.Contains(lx, ly)) return HIT_BTN_APPLY;

    // Record Button
    RectF rcBtnRec(168.0f, 410.0f, 150.0f, 42.0f);
    if (rcBtnRec.Contains(lx, ly)) return HIT_BTN_RECORD;

    // Analyze Button
    RectF rcBtnAnalyze(326.0f, 410.0f, 194.0f, 42.0f);
    if (rcBtnAnalyze.Contains(lx, ly)) return HIT_BTN_ANALYZE;

    // Open Folder Button
    RectF rcBtnFolder(20.0f, 460.0f, 150.0f, 36.0f);
    if (rcBtnFolder.Contains(lx, ly)) return HIT_BTN_FOLDER;

    // Minimize Button
    RectF rcBtnMin(178.0f, 460.0f, 140.0f, 36.0f);
    if (rcBtnMin.Contains(lx, ly)) return HIT_BTN_MINIMIZE;

    return HIT_NONE;
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
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFpsMenu, L"Target Framerate");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_RECORD, L"Record Screen (Snipping Tool)");
    AppendMenuW(hMenu, MF_STRING, IDM_FOLDER, L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_ANALYZE, L"Analyze Latest Video");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_RESET_ZOOM, L"Reset Zoom to 100%");
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
            break;
        }

        case WM_MOUSEWHEEL: {
            // Zoom In / Zoom Out with Ctrl + Mouse Wheel
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (zDelta > 0) {
                    // Zoom In
                    if (g_ZoomFactor < 2.45f) {
                        g_ZoomFactor += 0.10f;
                        if (g_ZoomFactor > 2.5f) g_ZoomFactor = 2.5f;
                        ApplyZoomWindowSize(hWnd);
                        SaveSettings();
                    }
                } else if (zDelta < 0) {
                    // Zoom Out
                    if (g_ZoomFactor > 0.75f) {
                        g_ZoomFactor -= 0.10f;
                        if (g_ZoomFactor < 0.75f) g_ZoomFactor = 0.75f;
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

            // Track mouse leaves for clean un-hovering
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
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_QUALITY_PRESET && data >= 0 && data < g_NumQualityOptions) {
                g_QualityLevel = data;
                SaveSettings();
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_AUTOSTART) {
                g_AutoStart = !g_AutoStart;
                SetAutoStart(g_AutoStart);
                InvalidateRect(hWnd, NULL, FALSE);
            } else if (hit == HIT_BTN_APPLY) {
                SaveSettings();
                MessageBoxW(hWnd, L"Settings saved successfully!\nSnipping Tool will record with the new framerate immediately.",
                            L"High Snipping Frames", MB_ICONINFORMATION);
            } else if (hit == HIT_BTN_RECORD) {
                LaunchSnippingTool();
            } else if (hit == HIT_BTN_FOLDER) {
                OpenRecordingsFolder();
            } else if (hit == HIT_BTN_ANALYZE) {
                AnalyzeLatestRecording(hWnd);
            } else if (hit == HIT_BTN_MINIMIZE) {
                ShowWindow(hWnd, SW_HIDE);
            }
            break;
        }

        case WM_ERASEBKGND:
            return 1; // Handled in double-buffered WM_PAINT to prevent flicker

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            if (w > 0 && h > 0) {
                // High-performance double buffering: allocate GDI+ bitmap backbuffer
                Bitmap backBuffer(w, h, PixelFormat32bppARGB);
                Graphics g(&backBuffer);

                RenderUI(g, w, h);

                // Blit backbuffer onto screen DC
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

                case IDM_FPS_AUTO: g_CurrentFPS = 0; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_60:   g_CurrentFPS = 60; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_90:   g_CurrentFPS = 90; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_120:  g_CurrentFPS = 120; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_144:  g_CurrentFPS = 144; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_165:  g_CurrentFPS = 165; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;
                case IDM_FPS_240:  g_CurrentFPS = 240; SaveSettings(); InvalidateRect(hWnd, NULL, FALSE); break;

                case IDM_RESET_ZOOM:
                    g_ZoomFactor = 1.0f;
                    ApplyZoomWindowSize(hWnd);
                    SaveSettings();
                    break;

                case IDM_RESTART_SVC: {
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
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    // Single instance mutex
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\HighSnippingFramesGUIMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExist = FindWindowW(L"HighSnippingFramesGUIClass", NULL);
        if (hExist) {
            ShowWindow(hExist, SW_SHOW);
            SetForegroundWindow(hExist);
        }
        GdiplusShutdown(g_gdiplusToken);
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

    // Initial window size computed from zoom
    int w = (int)(BASE_WIDTH * g_ZoomFactor + 0.5f);
    int h = (int)(BASE_HEIGHT * g_ZoomFactor + 0.5f);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    // Window creation with resizeable frame
    g_hWndMain = CreateWindowExW(WS_EX_APPWINDOW, L"HighSnippingFramesGUIClass",
                                 L"High Snipping Frames - Control Center",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 x, y, w, h, NULL, NULL, hInstance, NULL);

    if (!g_hWndMain) {
        GdiplusShutdown(g_gdiplusToken);
        return 0;
    }

    // Set correct client size based on zoom factor
    ApplyZoomWindowSize(g_hWndMain);

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
    GdiplusShutdown(g_gdiplusToken);
    return 0;
}
