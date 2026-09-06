#include <Windows.h>
#include <TlHelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_PATH_UNICODE 32768

static DWORD FindGame(
    VOID
) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    PROCESSENTRY32W procEntry = {sizeof(procEntry)};

    DWORD dwPid = 0;

    if (INVALID_HANDLE_VALUE == hSnapshot) {
        return 0;
    }

    if (Process32FirstW(hSnapshot, &procEntry)) {
        do {
            if (0 == _wcsicmp(procEntry.szExeFile, L"momentum.exe")) {
                if (0 != dwPid) {
                    dwPid = 0;
                    break;
                }

                dwPid = procEntry.th32ProcessID;
            }
        } while (Process32NextW(hSnapshot, &procEntry));
    }

    CloseHandle(hSnapshot);

    return dwPid;
}

static DWORD RunChild(
    _In_ LPCWSTR wszExe,
    _In_ LPCWSTR wszArguments
) {
    HANDLE hProcessHeap = GetProcessHeap();

    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    PWCHAR wszCommand = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, MAX_PATH_UNICODE * sizeof(WCHAR));

    if (NULL == wszCommand) {
        return GetLastError();
    }

    DWORD dwExit = EXIT_FAILURE;

    STARTUPINFOW startupInfo = {.cb = sizeof(startupInfo)};

    PROCESS_INFORMATION procInfo = { 0 };

    swprintf_s(wszCommand, MAX_PATH_UNICODE, L"\"%s\" %s", wszExe, wszArguments);

    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;

    BOOL bRet = CreateProcessW(
        wszExe,
        wszCommand,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &startupInfo,
        &procInfo
    );

    HeapFree(hProcessHeap, 0, wszCommand);

    if (!bRet) {
        return GetLastError();
    }

    CloseHandle(procInfo.hThread);

    if (WAIT_OBJECT_0 == WaitForSingleObject(procInfo.hProcess, INFINITE)) {
        GetExitCodeProcess(procInfo.hProcess, &dwExit);
    }

    CloseHandle(procInfo.hProcess);

    return dwExit;
}

INT WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_ HINSTANCE hPrevInstance,
    _In_ PWSTR pCmdLine,
    _In_ INT nCmdShow
) {
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    WCHAR *wszName = NULL;
    DWORD dwError = ERROR_SUCCESS;
    HANDLE hProcessHeap = GetProcessHeap();

    if (NULL == hProcessHeap) {
        dwError = GetLastError();
        MessageBoxW(NULL, L"Failed to get process heap.", L"BoardStats", MB_ICONERROR);

        return dwError;
    }

    PWCHAR wszDirectory = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, MAX_PATH_UNICODE * sizeof(WCHAR));

    PWCHAR wszController = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, MAX_PATH_UNICODE * sizeof(WCHAR));

    PWCHAR wszArguments = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, MAX_PATH_UNICODE * sizeof(WCHAR));

    PWCHAR wszMessage = HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, 768 * sizeof(WCHAR));

    LPVOID *appHeap[] = {&wszDirectory, &wszController, &wszArguments, &wszMessage};

    BOOL bSessionStarted = FALSE;
    BOOL bErrorReported = FALSE;
    HANDLE hGame = NULL;
    HANDLE hOwner = NULL;
    DWORD dwPid = FindGame();

    for (DWORD i = 0; i < ARRAYSIZE(appHeap); ++i) {
        if (NULL == appHeap[i]) {
            for (DWORD j = 0; j < ARRAYSIZE(appHeap); ++j) {
                if (NULL != *appHeap[j]) {
                    HeapFree(hProcessHeap, 0, *appHeap[j]);
                    *appHeap[j] = NULL;
                }
            }
            MessageBoxW(NULL, L"Not enough memory to launch BoardStats.", L"BoardStats", MB_ICONERROR);

            return EXIT_FAILURE;
        }
    }

    if (0 != wcslen(pCmdLine)) {
        MessageBoxW(
            NULL,
            L"Double-click this launcher to start the internal HUD. Use boardstats_settings.exe to configure " L"its appearance and placement.",
            L"BoardStats",
            MB_OK | MB_ICONINFORMATION
        );
        goto _FINAL;
    }

    if (0 == dwPid) {
        MessageBoxW(
            NULL,
            L"Launch one Momentum instance and load a map first, then run this launcher again.",
            L"BoardStats",
            MB_ICONINFORMATION
        );
        bErrorReported = TRUE;
        dwError = ERROR_NOT_READY;
        goto _FINAL;
    }

    hGame = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwPid);

    if (NULL == hGame) {
        dwError = GetLastError();
        goto _FINAL;
    }

    swprintf_s(wszArguments, MAX_PATH_UNICODE, L"Local\\SurfDB.BoardStats.Launcher.%lu", dwPid);
    hOwner = CreateMutexW(NULL, FALSE, wszArguments);
    dwError = GetLastError();

    if (NULL == hOwner || ERROR_ALREADY_EXISTS == dwError) {
        goto _FINAL;
    }

    dwError = ERROR_SUCCESS;
    if (0 == GetModuleFileNameW(NULL, wszDirectory, MAX_PATH_UNICODE)) {
        dwError = GetLastError();
        goto _FINAL;
    }

    wszName = wcsrchr(wszDirectory, L'\\');
    if (NULL == wszName) {
        dwError = ERROR_BAD_PATHNAME;
        goto _FINAL;
    }
    wszName[1] = L'\0';

    swprintf_s(wszController, MAX_PATH_UNICODE, L"%sController.exe", wszDirectory);
    swprintf_s(wszArguments, MAX_PATH_UNICODE, L"start %lu", dwPid);

    dwError = RunChild(wszController, wszArguments);
    if (ERROR_SUCCESS != dwError) {
        goto _FINAL;
    }

    swprintf_s(wszArguments, MAX_PATH_UNICODE, L"wait %lu", dwPid);
    dwError = RunChild(wszController, wszArguments);

    bSessionStarted = TRUE;

    // This retained handle identifies the original process even if its PID is reused.
    if (WAIT_TIMEOUT == WaitForSingleObject(hGame, 0)) {
        DWORD dwStopError = ERROR_SUCCESS;

        swprintf_s(wszArguments, MAX_PATH_UNICODE, L"stop %lu", dwPid);
        dwStopError = RunChild(wszController, wszArguments);

        if (ERROR_SUCCESS != dwStopError) {
            dwError = dwStopError;
        }
    }
_FINAL:
    // A remote waiter can end before Windows signals the terminating game process.
    if (bSessionStarted && hGame != NULL && ERROR_SUCCESS != dwError) {
        if (WAIT_OBJECT_0 == WaitForSingleObject(hGame, 2000)) {
            dwError = ERROR_SUCCESS;
        }
    }

    if (NULL != hOwner) {
        CloseHandle(hOwner);
    }

    if (NULL != hGame) {
        CloseHandle(hGame);
    }

    if (ERROR_SUCCESS != dwError && !bErrorReported) {
        swprintf_s(
            wszMessage,
            768,
            L"BoardStats launch/session failed (code %lu).\n\nSee README.md or run Controller.exe for " L"controller diagnostics. " L"Check that a map is loaded and no session is already active. After upgrading a loaded " L"collector, restart Momentum Mod once. " L"Run at the same elevation as the game.",
            dwError
        );
        MessageBoxW(NULL, wszMessage, L"BoardStats", MB_ICONERROR);
    }

    for (DWORD i = 0; i < ARRAYSIZE(appHeap); ++i) {
        if (NULL != *appHeap[i]) {
            HeapFree(hProcessHeap, 0, *appHeap[i]);
            *appHeap[i] = NULL;
        }
    }

    return ERROR_SUCCESS == dwError ? 0 : 1;
}
