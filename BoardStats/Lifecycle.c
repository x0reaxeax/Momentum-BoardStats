// Public lifecycle for the single DLL. Initialization never runs under DllMain.
#include <Windows.h>

#include "Components.h"

static SRWLOCK g_SessionLock = SRWLOCK_INIT;

__declspec(dllexport) DWORD WINAPI BoardStatsStart(
    _In_ LPVOID lpConfig
) {
    DWORD dwError = ERROR_BUSY;

    if (NULL == lpConfig) {
        return ERROR_INVALID_PARAMETER;
    }

    if (!TryAcquireSRWLockExclusive(&g_SessionLock)) {
        return dwError;
    }
    dwError = CollectorStart(NULL);
    if (ERROR_SUCCESS == dwError) {
        dwError = InternalStart(lpConfig);
        if (dwError != ERROR_SUCCESS) {
            InternalStop(NULL);
            CollectorStop(NULL);
        }
    }
    ReleaseSRWLockExclusive(&g_SessionLock);

    return dwError;
}

__declspec(dllexport) DWORD WINAPI BoardStatsStop(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);

    DWORD dwError = ERROR_BUSY;
    DWORD dwCollector = ERROR_SUCCESS;

    if (!TryAcquireSRWLockExclusive(&g_SessionLock)) {
        return dwError;
    }
    dwError = InternalStop(lpUnused);
    dwCollector = CollectorStop(NULL);
    if (ERROR_NOT_READY == dwError) {
        dwError = ERROR_SUCCESS;
    }

    if (ERROR_SUCCESS == dwError && dwCollector != ERROR_NOT_READY) {
        dwError = dwCollector;
    }
    ReleaseSRWLockExclusive(&g_SessionLock);

    return dwError;
}

__declspec(dllexport) DWORD WINAPI BoardStatsWait(
    _In_ LPVOID lpUnused
) {
    return InternalWait(lpUnused);
}
__declspec(dllexport) DWORD WINAPI BoardStatsReload(
    _In_ LPVOID lpUnused
) {
    return InternalReload(lpUnused);
}
__declspec(dllexport) DWORD WINAPI BoardStatsStatus(
    _In_ LPVOID lpUnused
) {
    return InternalStatus(lpUnused);
}
__declspec(dllexport) DWORD WINAPI BoardStatsError(
    _In_ LPVOID lpUnused
) {
    return InternalError(lpUnused);
}
