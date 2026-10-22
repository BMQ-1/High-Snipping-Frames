#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Find installed SnippingTool.exe path
static BOOL FindInstalledSnippingToolPath(WCHAR* outPath, DWORD maxLen) {
    WCHAR searchPath[MAX_PATH] = L"C:\\Program Files\\WindowsApps\\Microsoft.ScreenSketch_*_x64__8wekyb3d8bbwe";
    WIN32_FIND_DATAW wfd;
    HANDLE hFind = FindFirstFileW(searchPath, &wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
        swprintf(outPath, maxLen, L"C:\\Program Files\\WindowsApps\\%s\\SnippingTool\\SnippingTool.exe", wfd.cFileName);
        FindClose(hFind);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
    }

    WCHAR localApp[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH)) {
        swprintf(outPath, maxLen, L"%s\\Microsoft\\WindowsApps\\SnippingTool.exe", localApp);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            return TRUE;
        }
    }
    return FALSE;
}

// Find PID by process name
static DWORD FindPid(const WCHAR* name) {
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

// Inject DLL into target process
static BOOL InjectDLLByPID(DWORD pid, const WCHAR* dllPath) {
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) return FALSE;

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
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

    FILE* fp2 = _wfopen(L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\loader.log", L"a");
    if (fp2) {
        fwprintf(fp2, L"InjectDLLByPID: pid=%lu, dllPath=%s, exitCode=0x%08lX\n", pid, dllPath, exitCode);
        fclose(fp2);
    }

    // exitCode is the HMODULE (non-zero on success)
    return (exitCode != 0 && exitCode != 0xC0000409);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    FILE* fp = _wfopen(L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\loader.log", L"a");
    if (fp) {
        fwprintf(fp, L"LOADER CMD: %s\n", GetCommandLineW());
        fclose(fp);
    }

    WCHAR selfDir[MAX_PATH];
    GetModuleFileNameW(NULL, selfDir, MAX_PATH);
    WCHAR* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *pSlash = L'\0';

    WCHAR dllPath[MAX_PATH];
    swprintf(dllPath, MAX_PATH, L"%s\\SnippingToolHighFPS.dll", selfDir);
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wcscpy(dllPath, L"C:\\AntiGravity\\PC Tool\\Snipping Tool high FPS\\bin\\SnippingToolHighFPS.dll");
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    WCHAR targetCmdLine[4096] = { 0 };
    BOOL isClipOnly = FALSE;

    if (argc >= 2) {
        // IFEO mode: argv[0] is Launcher, argv[1] is target executable, argv[2..] are arguments
        const WCHAR* pFullCmd = GetCommandLineW();

        BOOL inQuote = FALSE;
        const WCHAR* p = pFullCmd;
        while (*p && (inQuote || *p != L' ')) {
            if (*p == L'"') inQuote = !inQuote;
            p++;
        }
        while (*p == L' ') p++;

        wcsncpy(targetCmdLine, p, 4095);

        // Check if launch is strictly for screenshot snip (Win + Shift + S)
        for (int i = 2; i < argc; i++) {
            if (wcsstr(argv[i], L"/clip") || wcsstr(argv[i], L"ms-screenclip") || wcsstr(argv[i], L"PrintScreen")) {
                isClipOnly = TRUE;
                break;
            }
        }
    } else {
        // Standalone mode: started directly
        WCHAR installedExe[MAX_PATH];
        if (FindInstalledSnippingToolPath(installedExe, MAX_PATH)) {
            swprintf(targetCmdLine, 4096, L"\"%s\"", installedExe);
        } else {
            MessageBoxW(NULL, L"Could not locate Windows Snipping Tool installation.",
                        L"Snipping Tool High FPS", MB_ICONERROR);
            if (argv) LocalFree(argv);
            return 1;
        }
    }

    if (argv) LocalFree(argv);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // If strictly screenshot snip, launch directly without injection
    // Guarantees Win + Shift + S is 100% instant, clean, and never freezes!
    if (isClipOnly) {
        if (CreateProcessW(NULL, targetCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return 0;
    }

    // Launch for editor / recording
    if (!CreateProcessW(NULL, targetCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Poll for the initialized SnippingTool.exe process and inject High-FPS module
    for (int retry = 0; retry < 25; retry++) {
        Sleep(150);
        DWORD pid = FindPid(L"SnippingTool.exe");
        if (pid != 0) {
            if (InjectDLLByPID(pid, dllPath)) {
                break; // Injected successfully!
            }
        }
    }

    return 0;
}
