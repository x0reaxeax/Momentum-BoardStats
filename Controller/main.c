// Explicit start/stop controller for the local, version-locked collector.
#include <Windows.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "Core.h"

static BOOL FindModule(
    _In_ DWORD dwPid,
    _In_ LPCWSTR lpszName,
    _Out_ MODULEENTRY32W *lpModule
) {
    HANDLE hSnapshot = INVALID_HANDLE_VALUE;
    BOOL bFound = FALSE;

    for (DWORD dwTry = 0; dwTry < 3U; ++dwTry) {
        hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, dwPid);
        if (INVALID_HANDLE_VALUE != hSnapshot || ERROR_BAD_LENGTH != GetLastError()) {
            break;
        }
    }

    if (INVALID_HANDLE_VALUE == hSnapshot) {
        return FALSE;
    }
    lpModule->dwSize = sizeof(*lpModule);
    if (Module32FirstW(hSnapshot, lpModule)) {
        do {
            if (0 == _wcsicmp(lpModule->szModule, lpszName)) {
                bFound = TRUE;
                break;
            }
        } while (Module32NextW(hSnapshot, lpModule));
    }
    CloseHandle(hSnapshot);
    if (!bFound) {
        SetLastError(ERROR_MOD_NOT_FOUND);
    }

    return bFound;
}

static DWORD CallRemote(
    _In_ HANDLE hProcess,
    _In_ ULONG_PTR ulFunction,
    _In_opt_ LPCWSTR lpszArgument,
    _Out_ DWORD *lpExit,
    _In_ DWORD dwTimeout
) {
    LPVOID lpRemote = NULL;
    HANDLE hThread = NULL;
    SIZE_T cbArgument = 0;
    SIZE_T cbWritten = 0;
    DWORD dwError = ERROR_SUCCESS;
    DWORD dwWait = 0;

    if (NULL == hProcess || NULL == lpExit) {
        return ERROR_INVALID_PARAMETER;
    }

    *lpExit = 0;

    if (NULL != lpszArgument) {
        cbArgument = (wcslen(lpszArgument) + 1) * sizeof(WCHAR);
        
        lpRemote = VirtualAllocEx(
            hProcess, 
            NULL, 
            cbArgument, 
            MEM_COMMIT | MEM_RESERVE, 
            PAGE_READWRITE
        );

        if (NULL == lpRemote) {
            return GetLastError();
        }

        if (!WriteProcessMemory(hProcess, lpRemote, lpszArgument, cbArgument, &cbWritten) ||
            cbArgument != cbWritten) {
            dwError = GetLastError();
            if (ERROR_SUCCESS == dwError) {
                dwError = ERROR_WRITE_FAULT;
            }
            goto _FINAL;
        }
    }

    hThread = CreateRemoteThread(
        hProcess, 
        NULL, 
        0, 
        (LPTHREAD_START_ROUTINE) ulFunction, 
        lpRemote, 
        0, 
        NULL
    );

    if (NULL == hThread) {
        dwError = GetLastError();
        goto _FINAL;
    }

    dwWait = WaitForSingleObject(hThread, dwTimeout);
    if (WAIT_OBJECT_0 != dwWait) {
        // The remote thread may still read this memory; never free it on timeout.
        fprintf(stderr, "[-] Remote call still pending; argument memory retained. Do not retry blindly.\n");
        lpRemote = NULL;
        dwError = WAIT_TIMEOUT == dwWait ? ERROR_TIMEOUT : GetLastError();
        goto _FINAL;
    }

    if (!GetExitCodeThread(hThread, lpExit)) {
        dwError = GetLastError();
    }
_FINAL:
    if (NULL != hThread) {
        CloseHandle(hThread);
    }

    if (NULL != lpRemote) {
        VirtualFreeEx(hProcess, lpRemote, 0, MEM_RELEASE);
    }

    return dwError;
}

static DWORD ResolveRemoteLoader(
    _In_ DWORD dwPid,
    _Out_ ULONG_PTR *lpFunction
) {
    HMODULE hOwner = NULL;

#pragma warning(push)
#pragma warning(disable: 6387) // just stfu
    FARPROC lpLoader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
#pragma warning(pop)
    LPWSTR wszOwner = NULL;
    LPWSTR wszName = NULL;

    DWORD dwError = ERROR_SUCCESS;

    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    wszOwner = HeapAlloc(
        hProcessHeap, 
        HEAP_ZERO_MEMORY, 
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    // i forgot where tf i slapped `HEAP_GENERATE_EXCEPTIONS`, so check
    if (NULL == wszOwner) {
        return ERROR_OUTOFMEMORY;
    }

    MODULEENTRY32W module = { 0 };
    if (NULL == lpLoader) {
        dwError = GetLastError();
        goto  _FINAL;
    }

    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR) lpLoader,
            &hOwner
        )) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (0 == GetModuleFileNameW(hOwner, wszOwner, MAX_PATH_UNICODE)) {
        dwError = GetLastError();
        goto _FINAL;
    }
    wszName = wcsrchr(wszOwner, L'\\');
    if (NULL == wszName || !FindModule(dwPid, wszName + 1, &module)) {
        dwError = ERROR_MOD_NOT_FOUND;
        goto _FINAL;
    }

    if (0 != _wcsicmp(wszOwner, module.szExePath)) {
        dwError = ERROR_REVISION_MISMATCH;
        goto _FINAL;
    }
    *lpFunction = (ULONG_PTR) module.modBaseAddr + ((ULONG_PTR) lpLoader - (ULONG_PTR) hOwner);

_FINAL:

    if (NULL != wszOwner) {
        HeapFree(hProcessHeap, 0, wszOwner);
    }

    return dwError;
}

int wmain(
    _In_ int argc,
    _In_ wchar_t **argv
) {
    DWORD dwPid = 0;
    LPWSTR wszEnd = NULL;
    HANDLE hProcess = NULL;
    HANDLE hCurrentProcessHeap = NULL;
    HMODULE hLocal = NULL;
    LPWSTR wszProcess = NULL;
    LPWSTR wszDll = NULL;
    LPWSTR wszConfig = NULL;
    DWORD cbProcess = MAX_PATH_UNICODE * sizeof(WCHAR);
    DWORD dwError = ERROR_SUCCESS;
    DWORD dwExit = 0;
    ULONG_PTR ulLoader = 0;

    FARPROC lpExport = NULL;
    MODULEENTRY32W server = { 0 };
    MODULEENTRY32W collector = { 0 };
    CHAR szHash[65] = { 0 };
    BOOL bStart = FALSE;
    BOOL bCheck = FALSE;
    BOOL bInternal = FALSE;
    LPCSTR lpszExport = NULL;
    LPCWSTR lpszDllName = L"BoardStats.dll";
    WCHAR *lpszName = NULL;

    if (argc >= 2) {
        if (0 == wcscmp(argv[1], L"start")) {
            lpszExport = "BoardStatsStart";
        }

        if (0 == wcscmp(argv[1], L"stop")) {
            lpszExport = "BoardStatsStop";
        }

        if (0 == wcscmp(argv[1], L"reload")) {
            lpszExport = "BoardStatsReload";
        }

        if (0 == wcscmp(argv[1], L"status")) {
            lpszExport = "BoardStatsStatus";
        }

        if (0 == wcscmp(argv[1], L"wait")) {
            lpszExport = "BoardStatsWait";
        }

        if (0 == wcscmp(argv[1], L"error")) {
            lpszExport = "BoardStatsError";
        }
        bInternal = lpszExport != NULL;
    }

    if (3 > argc || 4 < argc ||
        (!bInternal && (0 != wcscmp(argv[1], L"start") && 0 != wcscmp(argv[1], L"stop") &&
                        0 != wcscmp(argv[1], L"check")))) {
        fwprintf(
            stderr,
            L"Usage: Controller check|start|stop PID\n       Controller start PID [settings.ini]\n       " L"Controller stop|reload|status|error|wait PID\n"
        );

        return EXIT_FAILURE;
    }

    if (4 == argc && 0 != wcscmp(argv[1], L"start")) {
        return EXIT_FAILURE;
    }

    dwPid = wcstoul(argv[2], &wszEnd, 10);
    if (0 == dwPid || L'\0' != *wszEnd || L'-' == argv[2][0]) {
        return EXIT_FAILURE;
    }

    hCurrentProcessHeap = GetProcessHeap();
    if (NULL == hCurrentProcessHeap) {
        dwError = GetLastError();
        goto _FINAL;
    }

    LPWSTR *aAllocs[] = { 
        &wszProcess, 
        &wszDll, 
        &wszConfig 
    };

    for (DWORD i = 0; i < ARRAYSIZE(aAllocs); ++i) {
        *aAllocs[i] = HeapAlloc(
            hCurrentProcessHeap,
            HEAP_ZERO_MEMORY,
            MAX_PATH_UNICODE * sizeof(WCHAR)
        );

        if (NULL == *aAllocs[i]) {
            dwError = GetLastError();
            goto _FINAL;
        }
    }

    bStart = 0 == wcscmp(argv[1], L"start");
    bCheck = 0 == wcscmp(argv[1], L"check");
    
    hProcess = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | 
        (bCheck ? 0 : PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE),
        FALSE,
        dwPid
    );

    if (NULL == hProcess) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (!QueryFullProcessImageNameW(hProcess, 0, wszProcess, &cbProcess)) {
        dwError = GetLastError();
        goto _FINAL;
    }

    lpszName = wcsrchr(wszProcess, L'\\');
    if (NULL == lpszName || 0 != _wcsicmp(lpszName + 1, L"momentum.exe")) {
        dwError = ERROR_BAD_ENVIRONMENT;
        goto _FINAL;
    }

    if (bStart || bCheck) {
        if (!FindModule(dwPid, L"server.dll", &server)) {
            dwError = GetLastError();
            goto _FINAL;
        }
        dwError = HashFile(server.szExePath, szHash);
        
        if (ERROR_SUCCESS != dwError) {
            goto _FINAL;
        }

        if (0 != strcmp(szHash, SERVER_BOARD_SERVER_HASH)) {
            dwError = ERROR_REVISION_MISMATCH;
            goto _FINAL;
        }

        printf("[+] Supported server.dll SHA-256: %s\n", szHash);
        if (bCheck) {
            goto _FINAL;
        }
    }

    if (0 == GetModuleFileNameW(NULL, wszDll, MAX_PATH_UNICODE)) {
        dwError = GetLastError();
        goto _FINAL;
    }

    lpszName = wcsrchr(wszDll, L'\\');
    if (NULL == lpszName) {
        dwError = ERROR_BAD_PATHNAME;
        goto _FINAL;
    }

    lpszName[1] = L'\0';
    if (bStart && bInternal) {
        if (4 == argc) {
            DWORD cbPath = GetFullPathNameW(argv[3], MAX_PATH_UNICODE, wszConfig, NULL);

            if (0 == cbPath || cbPath >= MAX_PATH_UNICODE) {
                dwError = ERROR_BAD_PATHNAME;
                goto _FINAL;
            }
        } else {
            swprintf_s(wszConfig, MAX_PATH_UNICODE, L"%sboardstats.ini", wszDll);
        }
    }

    wcscat_s(wszDll, MAX_PATH_UNICODE, lpszDllName);
    hLocal = LoadLibraryExW(wszDll, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (NULL == hLocal) {
        dwError = GetLastError();
        goto _FINAL;
    }

    lpExport = GetProcAddress(hLocal, lpszExport);
    if (NULL == lpExport) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (!FindModule(dwPid, lpszDllName, &collector)) {
        if (!bStart) {
            dwError = ERROR_MOD_NOT_FOUND;
            goto _FINAL;
        }
        dwError = ResolveRemoteLoader(dwPid, &ulLoader);
        if (ERROR_SUCCESS != dwError) {
            goto _FINAL;
        }
        dwError = CallRemote(hProcess, ulLoader, wszDll, &dwExit, 15000);
        if (ERROR_SUCCESS != dwError) {
            goto _FINAL;
        }
        // A thread exit code truncates HMODULE on x64. Enumerate the full address.
        if (!FindModule(dwPid, lpszDllName, &collector)) {
            dwError = ERROR_DLL_INIT_FAILED;
            goto _FINAL;
        }
    }

    if (0 != _wcsicmp(wszDll, collector.szExePath)) {
        dwError = ERROR_ALREADY_EXISTS;
        goto _FINAL;
    }
    // Reject a different DLL at the same path after a rebuild while the old copy is loaded.
    {
        IMAGE_DOS_HEADER dos = { 0 };

        IMAGE_NT_HEADERS64 nt = { 0 };
        CONST IMAGE_DOS_HEADER *lpLocalDos = (CONST IMAGE_DOS_HEADER *) hLocal;

        CONST IMAGE_NT_HEADERS64 *lpLocalNt =
            (CONST IMAGE_NT_HEADERS64 *) ((BYTE *) hLocal + lpLocalDos->e_lfanew);
        SIZE_T cbRead = 0;

        if (!ReadProcessMemory(hProcess, collector.modBaseAddr, &dos, sizeof(dos), &cbRead) ||
            IMAGE_DOS_SIGNATURE != dos.e_magic || 0 > dos.e_lfanew || 0x100000 < dos.e_lfanew ||
            !ReadProcessMemory(hProcess, collector.modBaseAddr + dos.e_lfanew, &nt, sizeof(nt), &cbRead) ||
            nt.FileHeader.TimeDateStamp != lpLocalNt->FileHeader.TimeDateStamp ||
            nt.OptionalHeader.SizeOfImage != lpLocalNt->OptionalHeader.SizeOfImage) {
            dwError = ERROR_REVISION_MISMATCH;
            goto _FINAL;
        }
    }
    dwError = CallRemote(
        hProcess,
        (ULONG_PTR) collector.modBaseAddr + ((ULONG_PTR) lpExport - (ULONG_PTR) hLocal),
        bStart && bInternal ? wszConfig : NULL,
        &dwExit,
        0 == wcscmp(argv[1], L"wait") ? INFINITE : 15000
    );
    if (ERROR_SUCCESS == dwError && bInternal &&
        (0 == wcscmp(argv[1], L"status") || 0 == wcscmp(argv[1], L"error"))) {
        printf("[+] %ls: %lu (0x%08lX)\n", argv[1], dwExit, dwExit);
        goto _FINAL;
    }

    if (ERROR_SUCCESS == dwError) {
        dwError = dwExit;
    }

    if (ERROR_SUCCESS == dwError) {
        if (bInternal) {
            wprintf(L"[+] %s succeeded.\n", argv[1]);
        } else if (bStart) {
            printf("[+] Internal collection started.\n");
        } else {
            printf("[+] Stopped. Hooks disabled; DLL retained for safe restart.\n");
        }
    }
_FINAL:

    for (DWORD i = 0; i < ARRAYSIZE(aAllocs); ++i) {
        if (NULL != *aAllocs[i]) {
            HeapFree(hCurrentProcessHeap, 0, *aAllocs[i]);
            *aAllocs[i] = NULL;
        }
    }

    if (NULL != hLocal) {
        FreeLibrary(hLocal);
    }

    if (NULL != hProcess) {
        CloseHandle(hProcess);
    }

    if (ERROR_SUCCESS != dwError) {
        fprintf(stderr, "[-] BoardStats failed: E%lu\n", dwError);
        if (ERROR_ALREADY_EXISTS == dwError) {
            fprintf(
                stderr,
                "[*] Collector already active, failed, or a different copy is loaded. Stop the active " "session first.\n"
            );
        }

        if (ERROR_REVISION_MISMATCH == dwError) {
            fprintf(
                stderr,
                "[*] Build mismatch. A loaded older collector requires one game restart after upgrading.\n"
            );
        }

        if (ERROR_ACCESS_DENIED == dwError) {
            fprintf(stderr, "[*] Run the controller at the same elevation as Momentum.\n");
        }

        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
