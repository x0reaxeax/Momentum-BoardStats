#include <Windows.h>
#include <intrin.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#include "Core.h"
#include "Components.h"
#include "MinHook.h"

typedef UINT64(__fastcall *Solve_t)(LPVOID, LPVOID, LPVOID, LPVOID);
typedef UINT64(__fastcall *Clip_t)(LPVOID, CONST BOARD_VECTOR *, CONST BOARD_VECTOR *, LPBOARD_VECTOR, FLOAT);
typedef UINT64(__fastcall *Apply_t)(LPVOID, LPVOID);
typedef UINT64(__fastcall *Board_t)(LPVOID, CONST BOARD_VECTOR *);

typedef struct _SOLVE_CONTEXT {
    struct _SOLVE_CONTEXT *lpParent;
    LPVOID lpMovement;
    LPVOID lpResult;
    BOOL bCaptured;
    LONG lEpoch;
    BOARD_EVENT Event;
} SOLVE_CONTEXT, *LPSOLVE_CONTEXT;

typedef struct _APPLY_CONTEXT {
    struct _APPLY_CONTEXT *lpParent;
    LPVOID lpResult;
    LPVOID lpMovement;
    LONG lEpoch;
} APPLY_CONTEXT, *LPAPPLY_CONTEXT;

static HMODULE g_hSelf = NULL;
static HMODULE g_hServer = NULL;
static HANDLE g_hStop = NULL;
static HANDLE g_hWorker = NULL;
static volatile LONG g_lState = 0;
static volatile LONG g_lCollect = 0;
static volatile LONG g_lEpoch = 0;

static SRWLOCK g_ApiLock = SRWLOCK_INIT;
static BOOL g_bHooksCreated = FALSE;
static HUD_STATE g_Display = { 0 };
static HUD_STATE g_InternalSnapshot = { 0 };

static SRWLOCK g_SnapshotLock = SRWLOCK_INIT;

static volatile LONG g_lStopError = ERROR_SUCCESS;
static volatile LONG64 g_qwSolves = 0;
static volatile LONG64 g_qwCandidates = 0;
static volatile LONG64 g_qwAccepted = 0;
static volatile LONG64 g_qwUnmatched = 0;
static volatile LONG64 g_qwDropped = 0;
static volatile LONG64 g_qwInvalid = 0;
static volatile LONG64 g_qwSequence = 0;

static SRWLOCK g_QueueLock = SRWLOCK_INIT;
static BOARD_EVENT g_aQueue[SERVER_BOARD_QUEUE_CAPACITY] = { 0 };
static DWORD g_dwRead = 0;
static DWORD g_dwCount = 0;

static Solve_t g_lpSolve = NULL;
static Clip_t g_lpClip = NULL;
static Apply_t g_lpApply = NULL;
static Board_t g_lpBoard = NULL;
static __declspec(thread) LPSOLVE_CONTEXT g_lpSolveContext = NULL;
static __declspec(thread) LPAPPLY_CONTEXT g_lpApplyContext = NULL;
static __declspec(thread) BOARD_PENDING g_aPending[SERVER_BOARD_PENDING_CAPACITY];
static __declspec(thread) LONG g_lThreadEpoch = 0;

static MH_STATUS SetCollectorHooks(
    _In_ BOOL bEnable
) {
    CONST DWORD adwSites[] = { 
        SERVER_BOARD_SOLVER_RVA, 
        SERVER_BOARD_CLIP_RVA, 
        SERVER_BOARD_APPLY_RVA, 
        SERVER_BOARD_SETTER_RVA 
    };

    MH_STATUS mHresult = MH_OK;
    for (DWORD i = 0; i < ARRAYSIZE(adwSites); ++i) {
        LPVOID lpSite = (LPBYTE) g_hServer + adwSites[i];
        MH_STATUS mhStatus = bEnable ? MH_EnableHook(lpSite) : MH_DisableHook(lpSite);
        
        if (MH_OK == mhStatus) { 
            continue; 
        }

        if (!bEnable && (MH_ERROR_DISABLED == mhStatus || MH_ERROR_NOT_CREATED == mhStatus)) { 
            continue; 
        }

        mHresult = mhStatus;
    }

    return mHresult;
}

static DWORD PublishState(VOID) {
    DWORD dwError = ERROR_BUSY;
    
    if (TryAcquireSRWLockExclusive(&g_SnapshotLock)) {
        g_InternalSnapshot = g_Display;
        ReleaseSRWLockExclusive(&g_SnapshotLock);
        dwError = ERROR_SUCCESS;
    }

    return dwError;
}

// Internal renderer calls this directly in the game process
BOOL WINAPI CollectorGetSnapshot(
    _Out_ LPHUD_STATE lpState
) {
    if (NULL == lpState) {
        return FALSE;
    }

    ZeroMemory(lpState, sizeof(HUD_STATE));

    if (!TryAcquireSRWLockShared(&g_SnapshotLock)) {
        return FALSE;
    }

    *lpState = g_InternalSnapshot;
    ReleaseSRWLockShared(&g_SnapshotLock);

    return TRUE;
}

static LONG RefreshEpoch(
    VOID
) {
    LONG lEpoch = InterlockedCompareExchange(&g_lEpoch, 0, 0);

    if (g_lThreadEpoch != lEpoch) {
        ZeroMemory(g_aPending, sizeof(g_aPending));
        g_lThreadEpoch = lEpoch;
    }

    return lEpoch;
}

static BOOL IsCurrentEpoch(
    _In_ LONG lEpoch
) {
    return lEpoch == InterlockedCompareExchange(&g_lEpoch, 0, 0);
}

static BOOL IsCollecting(
    VOID
) {
    return 0 != InterlockedCompareExchange(&g_lCollect, 0, 0);
}

static BOOL ReadTick(
    _Out_ INT *lpTick
) {
    if (NULL == lpTick) {
        return FALSE;
    }

    *lpTick = 0;

    __try {
        CONST BYTE *lpGlobals = *(CONST BYTE **) ((BYTE *) g_hServer + SERVER_READTICK_RVA);

        if (NULL == lpGlobals) {
            return FALSE;
        }
        *lpTick = *(CONST INT *) (lpGlobals + SERVER_CONTEXT_TICK_OFFSET);

        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

static VOID PushEvent(
    _In_ CONST BOARD_EVENT *lpEvent,
    _In_ LONG lEpoch
) {
    if (!IsCollecting()) {
        return;
    }

    if (!TryAcquireSRWLockExclusive(&g_QueueLock)) {
        InterlockedIncrement64(&g_qwDropped);

        return;
    }

    if (!IsCollecting() || !IsCurrentEpoch(lEpoch)) {
        ReleaseSRWLockExclusive(&g_QueueLock);

        return;
    }

    if (SERVER_BOARD_QUEUE_CAPACITY == g_dwCount) {
        InterlockedIncrement64(&g_qwDropped);
    } else {
        g_aQueue[(g_dwRead + g_dwCount) % SERVER_BOARD_QUEUE_CAPACITY] = *lpEvent;
        ++g_dwCount;
    }

    ReleaseSRWLockExclusive(&g_QueueLock);
}

static BOOL PopEvent(
    _Out_ LPBOARD_EVENT lpEvent
) {
    BOOL bFound = FALSE;

    if (NULL == lpEvent) {
        return FALSE;
    }

    ZeroMemory(lpEvent, sizeof(BOARD_EVENT));

    AcquireSRWLockExclusive(&g_QueueLock);

    if (0 != g_dwCount) {
        *lpEvent = g_aQueue[g_dwRead];
        g_dwRead = (g_dwRead + 1) % SERVER_BOARD_QUEUE_CAPACITY;
        --g_dwCount;
        bFound = TRUE;
    }

    ReleaseSRWLockExclusive(&g_QueueLock);

    return bFound;
}

static UINT64 __fastcall CaptureSolve(
    _In_ LPVOID lpMovement,
    _In_ LPVOID lpFirstDestination,
    _In_ LPVOID lpFirstTrace,
    _In_ LPVOID lpResult
) {
    SOLVE_CONTEXT ctx = { 0 };
    UINT64 qwResult = 0;

    if (!IsCollecting()) {
        return g_lpSolve(lpMovement, lpFirstDestination, lpFirstTrace, lpResult);
    }

    InterlockedIncrement64(&g_qwSolves);
    ctx.lEpoch = RefreshEpoch();
    ForgetResult(g_aPending, (ULONG_PTR) lpResult);
    ctx.lpParent = g_lpSolveContext;
    ctx.lpMovement = lpMovement;
    ctx.lpResult = lpResult;
    g_lpSolveContext = &ctx;

    qwResult = g_lpSolve(lpMovement, lpFirstDestination, lpFirstTrace, lpResult);

    g_lpSolveContext = ctx.lpParent;
    if (ctx.bCaptured && IsCurrentEpoch(ctx.lEpoch) && IsCollecting()) {
        __try {
            CONST BYTE *lpMove = *(CONST BYTE **) ((BYTE *) lpMovement + SERVER_CONTEXT_MOVEDATA_OFFSET);

            ctx.Event.vecFinal = *(CONST BOARD_VECTOR *) (lpMove + SERVER_CONTEXT_VELOCITY_3DVEC_OFFSET);
            if (!StoreCandidate(g_aPending, &ctx.Event)) {
                InterlockedIncrement64(&g_qwDropped);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement64(&g_qwInvalid);
        }
    }

    return qwResult;
}

static UINT64 __fastcall CaptureClip(
    _In_ LPVOID lpMovement,
    _In_ CONST BOARD_VECTOR *lpIncoming,
    _In_ CONST BOARD_VECTOR *lpNormal,
    _Out_ LPBOARD_VECTOR lpOutgoing,
    _In_ FLOAT fOverbounce
) {
    LPSOLVE_CONTEXT lpContext = g_lpSolveContext;
    BOOL bCapture = FALSE;
    UINT64 qwResult = 0;

    if (IsCollecting() && NULL != lpContext && IsCurrentEpoch(lpContext->lEpoch) && !lpContext->bCaptured &&
        lpMovement == lpContext->lpMovement && (BYTE *) g_hServer + SERVER_COPY_CLIP_OUTPUT_RVA == (BYTE *) _ReturnAddress()) {
        __try {
            if (
                1 == *((BYTE *) lpContext->lpResult + SERVER_RESULT_BOARD_FLAG_OFFSET) && 
                lpNormal->fValue[2] > 0.0F &&
                0.7F > lpNormal->fValue[2] && 
                ReadTick(&lpContext->Event.iTick)
            ) {
                lpContext->Event.vecIncoming = *lpIncoming;
                lpContext->Event.vecNormal = *lpNormal;
                lpContext->Event.fOverbounce = fOverbounce;
                lpContext->Event.ulPlayer = *(ULONG_PTR *) ((BYTE *) lpMovement + SERVER_CONTEXT_PLAYER_OFFSET);
                lpContext->Event.ulResult = (ULONG_PTR) lpContext->lpResult;
                lpContext->Event.dwThreadId = GetCurrentThreadId();
                bCapture = TRUE;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement64(&g_qwInvalid);
        }
    }

    qwResult = g_lpClip(lpMovement, lpIncoming, lpNormal, lpOutgoing, fOverbounce);
    if (bCapture) {
        LARGE_INTEGER qwCounter = { 0 };

        lpContext->Event.vecOutgoing = *lpOutgoing;
        QueryPerformanceCounter(&qwCounter);
        lpContext->Event.qwCounter = qwCounter.QuadPart;
        lpContext->bCaptured = TRUE;
        InterlockedIncrement64(&g_qwCandidates);
    }

    return qwResult;
}

static UINT64 __fastcall CaptureApply(
    _In_ LPVOID lpMovement,
    _In_ LPVOID lpResult
) {
    APPLY_CONTEXT ctx = { 0 };
    UINT64 qwResult = 0;

    if (!IsCollecting()) {
        return g_lpApply(lpMovement, lpResult);
    }

    ctx.lEpoch = RefreshEpoch();
    ctx.lpParent = g_lpApplyContext;
    ctx.lpMovement = lpMovement;
    ctx.lpResult = lpResult;
    g_lpApplyContext = &ctx;

    qwResult = g_lpApply(lpMovement, lpResult);
    g_lpApplyContext = ctx.lpParent;
    
    if (IsCurrentEpoch(ctx.lEpoch)) {
        ForgetResult(g_aPending, (ULONG_PTR) lpResult);
    }

    return qwResult;
}

static UINT64 __fastcall CaptureBoard(
    _In_ LPVOID lpPlayer,
    _In_ CONST BOARD_VECTOR *lpIncoming
) {
    BOARD_EVENT event = { 0 };
    INT iTick = 0;
    BOOL bFound = FALSE;
    LONG lEpoch = RefreshEpoch();
    UINT64 qwResult = 0;

    if (IsCollecting()) {
        __try {
            if (NULL != g_lpApplyContext && g_lpApplyContext->lEpoch == lEpoch &&
                (BYTE *) lpIncoming == (BYTE *) g_lpApplyContext->lpResult + SERVER_CONTEXT_TICK_OFFSET && ReadTick(&iTick)) {
                bFound = TakeCandidate(
                    g_aPending,
                    (ULONG_PTR) g_lpApplyContext->lpResult,
                    (ULONG_PTR) lpPlayer,
                    iTick,
                    lpIncoming,
                    &event
                );
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // blyat
            InterlockedIncrement64(&g_qwInvalid);
        }

        if (!bFound) {
            InterlockedIncrement64(&g_qwUnmatched);
        }
    }

    qwResult = g_lpBoard(lpPlayer, lpIncoming);
    if (bFound) {
        event.qwSequence = InterlockedIncrement64(&g_qwSequence);
        InterlockedIncrement64(&g_qwAccepted);
        PushEvent(&event, lEpoch);
    }

    return qwResult;
}

static VOID UpdateStats(
    _In_ BOOL bStopped
) {
    g_Display.dUnmatched = (double) InterlockedCompareExchange64(&g_qwUnmatched, 0, 0);
    g_Display.dDropped = (double) InterlockedCompareExchange64(&g_qwDropped, 0, 0);
    g_Display.dInvalid = (double) InterlockedCompareExchange64(&g_qwInvalid, 0, 0);
    g_Display.dStopError = InterlockedCompareExchange(&g_lStopError, 0, 0);
    if (bStopped) {
        g_Display.bStopped = TRUE;
    }

    if (!g_Display.bStopped) {
        g_Display.qwHeartbeat = GetTickCount64();
    }
    PublishState();
}

static VOID DrainQueue(
    VOID
) {
    BOARD_EVENT event = { 0 };

    while (PopEvent(&event)) {
        BOARD_METRICS metrics = { 0 };

        if (!CalculateMetrics(&event, &metrics)) {
            InterlockedIncrement64(&g_qwInvalid);
            continue;
        }
        // discard bs metrics before updating the display
        if (!isfinite(metrics.dRetention) || metrics.dRetention < 0 || metrics.dRetention > 1000000 ||
            metrics.dIncoming > 1e9 || metrics.dOutgoing > 1e9 || fabs(metrics.dHorizontalDelta) > 1e9 ||
            event.qwSequence <= 0
        ) {
            InterlockedIncrement64(&g_qwInvalid);
            continue;
        }

        if (0 == g_Display.dwHistoryCount || (UINT64) event.qwSequence > g_Display.aHistory[0].qwSequence) {
            HUD_BOARD board = { 0 };

            board.qwSequence = event.qwSequence;
            board.iTick = event.iTick;
            board.dDelta = metrics.dDelta;
            board.dRetention = metrics.dRetention;
            board.dAngle = metrics.dAngle;
            board.dIncoming = metrics.dIncoming;
            board.dOutgoing = metrics.dOutgoing;
            board.dHorizontal = metrics.dHorizontalDelta;
            memmove(
                &g_Display.aHistory[1],
                &g_Display.aHistory[0],
                sizeof(HUD_BOARD) * (HUD_HISTORY_COUNT - 1)
            );
            g_Display.aHistory[0] = board;
            if (g_Display.dwHistoryCount < HUD_HISTORY_COUNT) {
                ++g_Display.dwHistoryCount;
            }
            g_Display.qwHeartbeat = GetTickCount64();
        }
        PublishState();
    }
}

static DWORD WINAPI RunCollector(
    _In_ LPVOID lpUnused
) {
    ULONGLONG qwLastStats = GetTickCount64();
    MH_STATUS nStatus = MH_OK;
    UNREFERENCED_PARAMETER(lpUnused);

    while (WAIT_TIMEOUT == WaitForSingleObject(g_hStop, 50)) {
        DrainQueue();
        PublishState();
        if (1000 <= GetTickCount64() - qwLastStats) {
            UpdateStats(FALSE);
            qwLastStats = GetTickCount64();
        }
    }

    InterlockedExchange(&g_lCollect, 0);
    // fence a producer that passed the first collecting check before stop
    AcquireSRWLockExclusive(&g_QueueLock);
    ReleaseSRWLockExclusive(&g_QueueLock);
    nStatus = SetCollectorHooks(FALSE);
    if (MH_OK != nStatus) {
        InterlockedExchange(&g_lStopError, ERROR_CAN_NOT_COMPLETE);
    }
    // keep them trampolines
    // no callback storage is freed, including on hook-disable failure
    DrainQueue();
    UpdateStats(TRUE);
    for (DWORD dwTry = 0; dwTry < 100; ++dwTry) {
        if (ERROR_BUSY != PublishState()) {
            break;
        }
        Sleep(1);
    }
    InterlockedExchange(&g_lState, MH_OK == nStatus ? 4 : 5);

    return (DWORD) InterlockedCompareExchange(&g_lStopError, 0, 0);
}

/// <summary>
/// Start or restart capture; called outside DllMain by the controller.
/// </summary>
/// <param name="lpUnused">Reserved; pass NULL.</param>
/// <returns>Win32 error code; zero means hooks and worker started.</returns>
static DWORD StartSession(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
#ifndef BOARD_COLLECTOR_TEST
    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    PWCHAR wszProcess = NULL;
    WCHAR *lpszName = NULL;
    HMODULE hPinned = NULL;
#endif
    DWORD dwError = ERROR_GEN_FAILURE;
    BOOL bHookLibrary = FALSE;
    LONG lState = InterlockedCompareExchange(&g_lState, 0, 0);

    if (0 != lState && 4 != lState) {
        return ERROR_ALREADY_EXISTS;
    }

    if (NULL != g_hWorker) {
        if (WAIT_OBJECT_0 != WaitForSingleObject(g_hWorker, 0)) {
            return ERROR_BUSY;
        }
        CloseHandle(g_hWorker);
        g_hWorker = NULL;
    }

    if (NULL != g_hStop) {
        CloseHandle(g_hStop);
        g_hStop = NULL;
    }

    InterlockedExchange(&g_lState, 1);
    InterlockedExchange(&g_lStopError, ERROR_SUCCESS);
    AcquireSRWLockExclusive(&g_QueueLock);
    InterlockedIncrement(&g_lEpoch);

    g_dwRead = 0;
    g_dwCount = 0;
    ReleaseSRWLockExclusive(&g_QueueLock);
#ifndef BOARD_COLLECTOR_TEST

    wszProcess = HeapAlloc(
        hProcessHeap, 
        HEAP_ZERO_MEMORY, 
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    if (NULL == wszProcess) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (0 == GetModuleFileNameW(NULL, wszProcess, MAX_PATH_UNICODE)) {
        dwError = ERROR_INVALID_PARAMETER;
        goto _FINAL;
    }

    lpszName = wcsrchr(wszProcess, L'\\');
    if (NULL == lpszName || 0 != _wcsicmp(lpszName + 1, L"momentum.exe")) {
        dwError = ERROR_BAD_ENVIRONMENT;
        goto _FINAL;
    }

    g_hServer = GetModuleHandleW(L"server.dll");
    dwError = ValidateServer(g_hServer);
    if (ERROR_SUCCESS != dwError) {
        goto _FINAL;
    }

    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR) g_hSelf,
        &hPinned
    )) {
        dwError = GetLastError();
        goto _FINAL;
    }

    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            (LPCWSTR) g_hServer,
            &hPinned
    )) {
        dwError = GetLastError();
        goto _FINAL;
    }

#endif

    g_hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (NULL == g_hStop) {
        dwError = GetLastError();
        goto _FINAL;
    }

    ZeroMemory(&g_Display, sizeof(g_Display));
    g_Display.dwExpectedPid = GetCurrentProcessId();
    g_Display.bSession = TRUE;
    g_Display.qwHeartbeat = GetTickCount64();
    PublishState();

    dwError = ERROR_INVALID_FUNCTION;
    if (!g_bHooksCreated) {
        MH_STATUS mhStatus = MH_Initialize();

        if (MH_OK != mhStatus && MH_ERROR_ALREADY_INITIALIZED != mhStatus) {
            goto _FINAL;
        }

        bHookLibrary = TRUE;
        if (MH_OK != MH_CreateHook(
            (BYTE *) g_hServer + SERVER_BOARD_SOLVER_RVA,
            (LPVOID) (ULONG_PTR) CaptureSolve,
            (LPVOID *) &g_lpSolve
        )) {
            goto _FINAL;
        }

        if (MH_OK != MH_CreateHook(
            (BYTE *) g_hServer + SERVER_BOARD_CLIP_RVA,
            (LPVOID) (ULONG_PTR) CaptureClip,
            (LPVOID *) &g_lpClip
        )) {
            goto _FINAL;
        }

        if (MH_OK != MH_CreateHook(
            (BYTE *) g_hServer + SERVER_BOARD_APPLY_RVA,
            (LPVOID) (ULONG_PTR) CaptureApply,
            (LPVOID *) &g_lpApply
        )) {
            goto _FINAL;
        }
        if (MH_OK != MH_CreateHook(
            (BYTE *) g_hServer + SERVER_BOARD_SETTER_RVA,
            (LPVOID) (ULONG_PTR) CaptureBoard,
            (LPVOID *) &g_lpBoard
        )) {
            goto _FINAL;
        }

        g_bHooksCreated = TRUE;
    }

    bHookLibrary = TRUE;
    if (MH_OK != SetCollectorHooks(TRUE)) {
        goto _FINAL;
    }

    InterlockedExchange(&g_lCollect, 1);

    g_hWorker = CreateThread(NULL, 0, RunCollector, NULL, 0, NULL);
    if (NULL == g_hWorker) {
        dwError = GetLastError();
        goto _FINAL;
    }

    InterlockedExchange(&g_lState, 2);

    return ERROR_SUCCESS;

_FINAL:
    InterlockedExchange(&g_lCollect, 0);

    if (bHookLibrary) {
        SetCollectorHooks(FALSE);
    }
    // intentionally retain any trampolines even after partial enable failure
    {
        g_Display.dwExpectedPid = GetCurrentProcessId();
        g_Display.bStopped = TRUE;
        g_Display.dStopError = dwError;
        PublishState();
    }

    // failure before touching hooks is ok but afterhook failures aint
    InterlockedExchange(&g_lState, bHookLibrary ? 5 : lState);
    
#ifndef BOARD_COLLECTOR_TEST
    if (NULL != wszProcess) {
        HeapFree(hProcessHeap, 0, wszProcess);
    }
#endif

    return dwError;
}

/// <summary>
/// Stop collection and wait for hook disable and queue drainage.
/// </summary>
/// <param name="lpUnused">Reserved; pass NULL.</param>
/// <returns>Win32 error code; zero means collection stopped.</returns>
static DWORD StopSession(
    _In_ LPVOID lpUnused
) {
    DWORD dwWait = 0;
    UNREFERENCED_PARAMETER(lpUnused);

    if (NULL == g_hWorker) {
        return ERROR_NOT_READY;
    }
    SetEvent(g_hStop);
    dwWait = WaitForSingleObject(g_hWorker, 10000);
    if (WAIT_OBJECT_0 != dwWait) {
        return WAIT_TIMEOUT == dwWait ? ERROR_TIMEOUT : GetLastError();
    }

    return (DWORD) InterlockedCompareExchange(&g_lStopError, 0, 0);
}

// Serialize remote lifecycle calls; a second controller fails instead of racing handles.
DWORD WINAPI CollectorStart(
    _In_ LPVOID lpUnused
) {
    DWORD dwError = ERROR_BUSY;

    if (!TryAcquireSRWLockExclusive(&g_ApiLock)) {
        return dwError;
    }
    dwError = StartSession(lpUnused);
    ReleaseSRWLockExclusive(&g_ApiLock);

    return dwError;
}

DWORD WINAPI CollectorStop(
    _In_ LPVOID lpUnused
) {
    DWORD dwError = ERROR_BUSY;

    if (!TryAcquireSRWLockExclusive(&g_ApiLock)) {
        return dwError;
    }
    dwError = StopSession(lpUnused);
    ReleaseSRWLockExclusive(&g_ApiLock);

    return dwError;
}

VOID CollectorAttach(
    _In_ HMODULE hModule
) {
    g_hSelf = hModule;
}
