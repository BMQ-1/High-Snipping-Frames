#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

static WCHAR g_LogPath[MAX_PATH] = { 0 };

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
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        Log("[SnippingToolDaemon] DLL path invalid or missing: %ls", dllPath);
        return FALSE;
    }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        Log("[SnippingToolDaemon] OpenProcess(PID %u) failed (err: %u)", pid, GetLastError());
        return FALSE;
    }

    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(WCHAR);

    LPVOID pRemoteBuf = VirtualAllocEx(hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemoteBuf) {
        Log("[SnippingToolDaemon] VirtualAllocEx failed");
        CloseHandle(hProcess);
        return FALSE;
    }

    if (!WriteProcessMemory(hProcess, pRemoteBuf, dllPath, pathBytes, NULL)) {
        Log("[SnippingToolDaemon] WriteProcessMemory failed");
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
        Log("[SnippingToolDaemon] CreateRemoteThread failed (err: %u)", GetLastError());
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

    Log("[SnippingToolDaemon] Remote LoadLibraryW returned module handle: 0x%08X in PID %u", exitCode, pid);
    return (exitCode != 0 && exitCode != 0xC0000409);
}

static void ScanAndInject(const WCHAR* dllPath) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"SnippingTool.exe") == 0) {
                if (!IsModuleLoaded(pe.th32ProcessID, L"SnippingToolHighFPS.dll")) {
                    Log("[SnippingToolDaemon] Detected SnippingTool.exe (PID %u) without HighFPS mod. Injecting...", pe.th32ProcessID);
                    if (InjectDLL(pe.th32ProcessID, dllPath)) {
                        Log("[SnippingToolDaemon] Injected HighFPS mod successfully into PID %u", pe.th32ProcessID);
                    }
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    WCHAR tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath)) {
        swprintf(g_LogPath, MAX_PATH, L"%sSnippingToolDaemon.log", tempPath);
    }

    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\SnippingToolHighFPSDaemonMutex_v2");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("[SnippingToolDaemon] Another instance is already running. Exiting.");
        return 0;
    }

    Log("[SnippingToolDaemon] Service started successfully (PID: %u)", GetCurrentProcessId());

    WCHAR selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *pSlash = L'\0';

    WCHAR dllPath[MAX_PATH];
    swprintf(dllPath, MAX_PATH, L"%s\\SnippingToolHighFPS.dll", selfDir);
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(dllPath, L"C:\\Program Files\\SnippingToolHighFPS\\SnippingToolHighFPS.dll");
    }
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(dllPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\High-Snipping-Frames\\bin\\SnippingToolHighFPS.dll");
    }

    Log("[SnippingToolDaemon] Monitoring for SnippingTool.exe (Target DLL: %ls)", dllPath);

    while (TRUE) {
        ScanAndInject(dllPath);
        Sleep(250);
    }

    if (hMutex) CloseHandle(hMutex);
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    return wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOW);
}
