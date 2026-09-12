// Redirect only local mid-run paint rays to the active free-camera entity.
// offseterino for mom 0.10.8
#include <Windows.h>
#include <intrin.h>

#include <math.h>
#include <string.h>

#include "Core.h"
#include "Paint.h"
#include "MinHook.h"

#define CLIENT_PAINT_EMITTER_RVA    0xD880U
#define CLIENT_PAINT_RETURN_RVA     0x5B3E2U
#define CLIENT_LOCAL_PLAYER_RVA     0xFD3C70U
#define CLIENT_ENTITY_TABLE_RVA     0xF64588U
#define CLIENT_CAMERA_VTABLE_RVA    0xC52B98U
#define CLIENT_ABS_ORIGIN_RVA       0x30BB80U
#define CLIENT_CAMERA_ANGLES_RVA    0x507620U
#define PLAYER_FREECAM_OFFSET       0x3D90U
#define PLAYER_VIEW_ENTITY_OFFSET   0x36A8U

typedef VOID (__fastcall *EmitPaint_t)(LPVOID, CONST BOARD_VECTOR *, CONST BOARD_VECTOR *, UINT, BYTE, INT, FLOAT);
typedef CONST BOARD_VECTOR *(__fastcall *CameraVector_t)(LPVOID);

static SRWLOCK g_PaintLock = SRWLOCK_INIT;
static EmitPaint_t g_lpEmitPaint = NULL;
static HMODULE g_hPaintClient = NULL;
static volatile LONG g_lPaintEnabled = 0;

// read-only stuff for external version + diagnostics
__declspec(dllexport) volatile LONG64 BoardStatsPaintCalls = 0;
__declspec(dllexport) volatile LONG64 BoardStatsPaintRedirects = 0;
__declspec(dllexport) volatile LONG64 BoardStatsPaintLastCaller = 0;
__declspec(dllexport) volatile LONG BoardStatsPaintLastType = 0;
__declspec(dllexport) volatile LONG BoardStatsPaintFailure = 0;

BOOL PaintShouldOverride(
    _In_ BOOL bEnabled,
    _In_ DWORD dwFreeCam,
    _In_ BOOL bLocalPlayer,
    _In_ BOOL bPaintCaller
) {
    return bEnabled && 1U == dwFreeCam && bLocalPlayer && bPaintCaller;
}

static BOOL ReadPaintCamera(
    _In_ LPVOID lpPlayer,
    _Out_ LPBOARD_VECTOR lpOrigin,
    _Out_ LPBOARD_VECTOR lpAngles
) {
    InterlockedExchange(&BoardStatsPaintFailure, 1);
    __try {
        LPBYTE lpBase = (LPBYTE) g_hPaintClient;
        BOOL bLocal = lpPlayer == *(LPVOID *) (lpBase + CLIENT_LOCAL_PLAYER_RVA);
        DWORD dwFreeCam = *((LPBYTE) lpPlayer + PLAYER_FREECAM_OFFSET);
        DWORD dwHandle = *(DWORD *) ((LPBYTE) lpPlayer + PLAYER_VIEW_ENTITY_OFFSET);
        LPBYTE lpTable = *(LPBYTE *) (lpBase + CLIENT_ENTITY_TABLE_RVA);
        LPBYTE lpEntry = NULL;
        LPVOID lpCamera = NULL;
        CameraVector_t lpGetOrigin = (CameraVector_t) (ULONG_PTR) (lpBase + CLIENT_ABS_ORIGIN_RVA);
        CameraVector_t lpGetAngles = (CameraVector_t) (ULONG_PTR) (lpBase + CLIENT_CAMERA_ANGLES_RVA);

        if (!PaintShouldOverride(TRUE, dwFreeCam, bLocal, TRUE) || MAXDWORD == dwHandle || NULL == lpTable) {
            return FALSE;
        }

        InterlockedExchange(&BoardStatsPaintFailure, 2);
        // CEntInfo: stride 32, entity pointer +8, handle serial +16.
        lpEntry = lpTable + 32U * (dwHandle & 0xFFFFU);
        if (*(DWORD *) (lpEntry + 16U) != (dwHandle >> 16)) {
            return FALSE;
        }

        InterlockedExchange(&BoardStatsPaintFailure, 3);
        lpCamera = *(LPVOID *) (lpEntry + 8U);
        if (NULL == lpCamera || *(LPVOID *) lpCamera != lpBase + CLIENT_CAMERA_VTABLE_RVA) {
            return FALSE;
        }

        InterlockedExchange(&BoardStatsPaintFailure, 4);
        // use the same entity accessors as the camera, on the game's paint thread.
        *lpOrigin = *lpGetOrigin(lpCamera);
        *lpAngles = *lpGetAngles(lpCamera);
        for (UINT i = 0; i < 3U; ++i) {
            if (!isfinite(lpOrigin->fValue[i]) || !isfinite(lpAngles->fValue[i])) {
                return FALSE;
            }
        }

        InterlockedExchange(&BoardStatsPaintFailure, 0);
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static VOID __fastcall EmitCameraPaint(
    _In_ LPVOID lpPlayer,
    _In_ CONST BOARD_VECTOR *lpOrigin,
    _In_ CONST BOARD_VECTOR *lpAngles,
    _In_ UINT uType,
    _In_ BYTE bMode,
    _In_ INT iSeed,
    _In_ FLOAT fSpread
) {
    BOARD_VECTOR vecOrigin = { 0 };
    BOARD_VECTOR vecAngles = { 0 };

    InterlockedIncrement64(&BoardStatsPaintCalls);
    InterlockedExchange64(&BoardStatsPaintLastCaller, (LONG64) (ULONG_PTR) _ReturnAddress());
    InterlockedExchange(&BoardStatsPaintLastType, (LONG) uType);

    if (
        5U == uType &&
        (LPBYTE) g_hPaintClient + CLIENT_PAINT_RETURN_RVA == _ReturnAddress() &&
        0 != InterlockedCompareExchange(&g_lPaintEnabled, 0, 0) &&
        NULL != lpPlayer&& 
        ReadPaintCamera(lpPlayer, &vecOrigin, &vecAngles)
    ) {
        InterlockedIncrement64(&BoardStatsPaintRedirects);
        lpOrigin = &vecOrigin;
        lpAngles = &vecAngles;
    }
    g_lpEmitPaint(lpPlayer, lpOrigin, lpAngles, uType, bMode, iSeed, fSpread);
}

static DWORD InitializePaintHook(
    VOID
) {
    PWCHAR wszPath = NULL;
    CHAR szHash[65] = { 0 };
    HMODULE hPinned = NULL;
    DWORD dwLength = 0;
    DWORD dwError = ERROR_SUCCESS;
    CONST BYTE abGetter[] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0xB8 };
    CONST BYTE abCaller[] = { 0xE8, 0x9E, 0x24, 0xFB, 0xFF };
    MH_STATUS mhStatus = MH_OK;
    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    wszPath = (PWCHAR) HeapAlloc(
        hProcessHeap, 
        HEAP_ZERO_MEMORY, 
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    if (NULL == wszPath) {
        return ERROR_OUTOFMEMORY;
    }

    g_hPaintClient = GetModuleHandleW(L"client.dll");
    if (NULL == g_hPaintClient) {
        dwError = ERROR_MOD_NOT_FOUND;
        goto _FINAL;
    }

    dwLength = GetModuleFileNameW(g_hPaintClient, wszPath, MAX_PATH_UNICODE);
    if (0 == dwLength || dwLength >= MAX_PATH_UNICODE) {
        dwError = ERROR_INSUFFICIENT_BUFFER;
        goto _FINAL;
    }

    dwError = HashFile(wszPath, szHash);
    if (ERROR_SUCCESS != dwError) {
        goto _FINAL;
    }

    if (0 != strcmp(szHash, "abb9b0c3686d5ac7e3b355d5e7209b9a3a55ba7d4d7772665e17a32605c7c6f3")) {
        dwError = ERROR_REVISION_MISMATCH;
        goto _FINAL;
    }

    __try {
        if (0 != memcmp((LPBYTE) g_hPaintClient + CLIENT_PAINT_EMITTER_RVA, abGetter, sizeof(abGetter))) {
            dwError = ERROR_INVALID_DATA;
            goto _FINAL;
        }

        if (0 != memcmp((LPBYTE) g_hPaintClient + CLIENT_PAINT_RETURN_RVA - 5U, abCaller, sizeof(abCaller))) {
            dwError = ERROR_INVALID_DATA;
            goto _FINAL;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dwError = ERROR_INVALID_ADDRESS;
        goto _FINAL;
    }

    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR) g_hPaintClient,
        &hPinned
    )) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR) (ULONG_PTR) EmitCameraPaint,
        &hPinned
    )) {
        dwError = GetLastError();
        goto _FINAL;
    }

    mhStatus = MH_Initialize();
    if (MH_OK != mhStatus && MH_ERROR_ALREADY_INITIALIZED != mhStatus) {
        dwError = ERROR_CAN_NOT_COMPLETE;
        goto _FINAL;
    }

    if (MH_OK != MH_CreateHook(
        (LPBYTE) g_hPaintClient + CLIENT_PAINT_EMITTER_RVA,
        (LPVOID) (ULONG_PTR) EmitCameraPaint,
        (LPVOID *) &g_lpEmitPaint
    )) {
        dwError = ERROR_CAN_NOT_COMPLETE;
        goto _FINAL;
    }

_FINAL:
    if (NULL != wszPath) {
        HeapFree(hProcessHeap, 0, wszPath);
        wszPath = NULL;
    }

    return dwError;
}

DWORD ConfigurePaint(
    _In_ BOOL bEnabled
) {
    DWORD dwError = ERROR_SUCCESS;
    MH_STATUS mhStatus = MH_OK;

    AcquireSRWLockExclusive(&g_PaintLock);
    InterlockedExchange(&g_lPaintEnabled, 0);
    if (bEnabled && NULL == g_lpEmitPaint) {
        dwError = InitializePaintHook();
    }

    if (ERROR_SUCCESS == dwError && g_lpEmitPaint != NULL) {
        LPVOID lpTarget = (LPBYTE) g_hPaintClient + CLIENT_PAINT_EMITTER_RVA;

        mhStatus = bEnabled ? MH_EnableHook(lpTarget) : MH_DisableHook(lpTarget);
        if (MH_OK != mhStatus && MH_ERROR_ENABLED != mhStatus && MH_ERROR_DISABLED != mhStatus) {
            dwError = ERROR_CAN_NOT_COMPLETE;
        }
    }

    if (ERROR_SUCCESS == dwError && bEnabled) {
        InterlockedExchange(&g_lPaintEnabled, 1);
    }

    ReleaseSRWLockExclusive(&g_PaintLock);
    return dwError;
}
