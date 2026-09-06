#ifndef _BOARDSTATS_CORE_H
#define _BOARDSTATS_CORE_H

#include <Windows.h>

/*
|---------------------------------------------------------------------|
| Inferred role                           | IDA VA        | RVA       |
|-----------------------------------------|---------------|-----------|
| DT_MOM_Player send-table initialization | `0x18008D690` | `0x8D690` |
| Ramp board velocity setter              | `0x18009BC20` | `0x9BC20` |
| Ramp leave velocity setter              | `0x18009BDB0` | `0x9BDB0` |
| Movement result application             | `0x18006F8D0` | `0x6F8D0` |
| Collision solver, TryPlayerMove-like    | `0x18006DB20` | `0x6DB20` |
| Solver/result wrapper                   | `0x18006D460` | `0x6D460` |
|---------------------------------------------------------------------|
*/

#define SERVER_BOARD_PENDING_CAPACITY           16U
#define SERVER_BOARD_QUEUE_CAPACITY             256U
#define SERVER_BOARD_SERVER_HASH                "a897daa64638e3f53fdfaf67950e9b8f6685000079a111f3672a4a4f75450829"

#define SERVER_BOARD_SOLVER_RVA                 0x6DB20U
#define SERVER_BOARD_CLIP_RVA                   0x73FB0U
#define SERVER_BOARD_APPLY_RVA                  0x6F8D0U
#define SERVER_BOARD_SETTER_RVA                 0x9BC20U
#define SERVER_BOARD_EVENT_FLAG_RVA             0x6F5A0U
#define SERVER_READTICK_RVA                     0xAC4408U
#define SERVER_COPY_CLIP_OUTPUT_RVA             0x6F5B2U


#define SERVER_RESULT_BOARD_FLAG_OFFSET         0x20U
#define SERVER_CONTEXT_TICK_OFFSET              0x24U
#define SERVER_CONTEXT_MOVEDATA_OFFSET          0x28U
#define SERVER_CONTEXT_PLAYER_OFFSET            0x30U
#define SERVER_CONTEXT_VELOCITY_3DVEC_OFFSET    0x48U

#define MAX_PATH_UNICODE                        32768

typedef double DOUBLE;

typedef struct _BOARD_VECTOR {
    FLOAT fValue[3];
} BOARD_VECTOR, *LPBOARD_VECTOR;

typedef struct _BOARD_EVENT {
    BOARD_VECTOR vecIncoming;
    BOARD_VECTOR vecNormal;
    BOARD_VECTOR vecOutgoing;
    BOARD_VECTOR vecFinal;
    FLOAT fOverbounce;
    INT iTick;
    DWORD dwThreadId;
    ULONG_PTR ulPlayer;
    ULONG_PTR ulResult;
    LONG64 qwSequence;
    LONG64 qwCounter;
} BOARD_EVENT, *LPBOARD_EVENT;

typedef struct _BOARD_METRICS {
    DOUBLE dIncoming;
    DOUBLE dOutgoing;
    DOUBLE dDelta;
    DOUBLE dRetention;
    DOUBLE dAngle;
    DOUBLE dHorizontalDelta;
} BOARD_METRICS, *LPBOARD_METRICS;

typedef struct _BOARD_PENDING {
    BOOL bValid;
    BOARD_EVENT Event;
} BOARD_PENDING, *LPBOARD_PENDING;

/// <summary>
/// Validate vectors and compute collision metrics without mutating the sample.
/// </summary>
/// <param name="lpEvent">Captured numeric sample.</param>
/// <param name="lpMetrics">Receives finite metrics.</param>
/// <returns>TRUE on valid nonzero input, FALSE on invalid geometry or numbers.</returns>
BOOL CalculateMetrics(
    _In_ CONST LPBOARD_EVENT lpEvent,
    _Out_ LPBOARD_METRICS lpMetrics
);
/// <summary>
/// Invalidate a reused result object before a new solve.
/// </summary>
/// <param name="lpPending">Fixed pending array.</param>
/// <param name="ulResult">Result identity.</param>
/// <returns>No value.</returns>
VOID ForgetResult(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ ULONG_PTR ulResult
);
/// <summary>
/// Store a candidate, expiring samples from other ticks.
/// </summary>
/// <param name="lpPending">Fixed pending array.</param>
/// <param name="lpEvent">Candidate to copy.</param>
/// <returns>TRUE when stored; FALSE if the array is full.</returns>
BOOL StoreCandidate(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ CONST LPBOARD_EVENT lpEvent
);
/// <summary>
/// Consume exactly one matching applied result, including velocity identity.
/// </summary>
/// <param name="lpPending">Fixed pending array.</param>
/// <param name="ulResult">Applied result identity.</param>
/// <param name="ulPlayer">Expected player identity.</param>
/// <param name="iTick">Current simulation tick.</param>
/// <param name="lpIncoming">Applied incoming vector.</param>
/// <param name="lpEvent">Receives the consumed sample.</param>
/// <returns>TRUE on a match; FALSE on missing or stale data.</returns>
BOOL TakeCandidate(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ ULONG_PTR ulResult,
    _In_ ULONG_PTR ulPlayer,
    _In_ INT iTick,
    _In_ CONST BOARD_VECTOR *lpIncoming,
    _Out_ LPBOARD_EVENT lpEvent
);
/// <summary>
/// Compute a file SHA-256 using Windows CNG.
/// </summary>
/// <param name="lpszPath">File to read.</param>
/// <param name="szHash">Receives 64 lowercase hex digits and a terminator.</param>
/// <returns>Win32 error code; ERROR_SUCCESS on success.</returns>
DWORD HashFile(
    _In_ LPCWSTR cwszPath,
    _Out_writes_(65) PCHAR szHash
);
/// <summary>
/// Check the known disk build and live hook entry bytes before patching.
/// </summary>
/// <param name="hServer">Loaded server module in this process.</param>
/// <returns>ERROR_SUCCESS only for the supported unmodified hook entries.</returns>
DWORD ValidateServer(
    _Inout_opt_ HMODULE hServer
);
#endif // _BOARDSTATS_CORE_H
