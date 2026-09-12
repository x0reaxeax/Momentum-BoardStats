// Read-only cl_yawspeed sampling for the hash-verified Momentum client.
#include <Windows.h>

#include <math.h>
#include <string.h>

#include "Core.h"
#include "YawSpeed.h"

#define CLIENT_YAWSPEED_RVA         0x111DD68U
#define CLIENT_CONVAR_VTABLE_RVA    0xC89208U
#define CONVAR_PARENT_OFFSET        0x38U
#define CONVAR_FLOAT_OFFSET         0x54U

// Accessed only while the renderer lock is held. Module references survive until process yeet.
static HMODULE g_hYawClient = NULL;
static BOOL g_bYawReady = FALSE;

VOID ConfigureYawSpeed(
    _In_ BOOL bEnabled
) {
    // maybe procheap handle should be global at this point tbh, imo, lfg, smd
    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return;
    }

    PWCHAR wszPath = HeapAlloc(
        hProcessHeap,
        HEAP_ZERO_MEMORY,
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    if (NULL == wszPath) {
        return;
    }

    CHAR szHash[65] = { 0 };
    HMODULE hClient = NULL;
    DWORD dwLength = 0;

    g_bYawReady = FALSE;
    if (!bEnabled) {
        goto _HEAPFREE;
    }
    if (NULL != g_hYawClient) {
        g_bYawReady = TRUE;
        goto _HEAPFREE;
    }
    if (!GetModuleHandleExW(0, L"client.dll", &hClient)) {
        goto _HEAPFREE;
    }
    dwLength = GetModuleFileNameW(hClient, wszPath, MAX_PATH_UNICODE);
    if (0 == dwLength || dwLength >= MAX_PATH_UNICODE ||
        ERROR_SUCCESS != HashFile(wszPath, szHash) ||
        0 != strcmp(szHash, "abb9b0c3686d5ac7e3b355d5e7209b9a3a55ba7d4d7772665e17a32605c7c6f3")) {
        FreeLibrary(hClient);
        goto _HEAPFREE;
    }
    g_hYawClient = hClient;
    g_bYawReady = TRUE;

_HEAPFREE:
    if (NULL != wszPath) {
        HeapFree(hProcessHeap, 0, wszPath);
    }
}

BOOL ReadYawSpeed(
    _Out_ FLOAT *lpValue
) {
    if (NULL == lpValue) {
        return FALSE;
    }

    *lpValue = 0;
    if (!g_bYawReady || NULL == g_hYawClient) {
        return FALSE;
    }
    __try {
        CONST BYTE *lpVariable = (CONST BYTE *) g_hYawClient + CLIENT_YAWSPEED_RVA;
        DWORD dwBits = 0;
        FLOAT fValue = 0;

        // Matches the inline GetFloat path in client+0x3F27A0.
        // redirected parent is intentionally yeeted rather than shooting an unverified virtual call.
        if (*(CONST BYTE *CONST *) (lpVariable + CONVAR_PARENT_OFFSET) != lpVariable ||
            *(CONST BYTE *CONST *) lpVariable != (CONST BYTE *) g_hYawClient + CLIENT_CONVAR_VTABLE_RVA) {
            return FALSE;
        }

        dwBits = *(volatile CONST DWORD *) (lpVariable + CONVAR_FLOAT_OFFSET) ^ (DWORD) (ULONG_PTR) lpVariable;
        memcpy(&fValue, &dwBits, sizeof(fValue));
        
        if (!isfinite(fValue)) {
            return FALSE;
        }

        *lpValue = fValue;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}
