#include <Windows.h>

#include <math.h>
#include <string.h>

#include "Core.h"

static DOUBLE GetLength(
    _In_ CONST LPBOARD_VECTOR lpVector
) {
    DOUBLE dSum = 0.0;

    for (DWORD dwIndex = 0; dwIndex < 3U; ++dwIndex) {
        dSum += (DOUBLE) lpVector->fValue[dwIndex] * lpVector->fValue[dwIndex];
    }

    return sqrt(dSum);
}

BOOL CalculateMetrics(
    _In_ CONST LPBOARD_EVENT lpEvent,
    _Out_ LPBOARD_METRICS lpMetrics
) {
    DOUBLE dNormal = 0.0;
    DOUBLE dDot = 0.0;
    DOUBLE dCos = 0.0;

    if (NULL == lpEvent || NULL == lpMetrics) {
        return FALSE;
    }

    ZeroMemory(lpMetrics, sizeof(BOARD_METRICS));

    dNormal = GetLength(&lpEvent->vecNormal);
    for (DWORD dwIndex = 0; dwIndex < 3U; ++dwIndex) {
        if (!isfinite(lpEvent->vecIncoming.fValue[dwIndex])) {
            return FALSE;
        } 
        if (!isfinite(lpEvent->vecOutgoing.fValue[dwIndex])) {
            return FALSE;
        } 
        if (!isfinite(lpEvent->vecNormal.fValue[dwIndex])) {
            return FALSE;
        } 
        if (!isfinite(lpEvent->vecFinal.fValue[dwIndex])) {
            return FALSE;
        }
        dDot += (DOUBLE) lpEvent->vecIncoming.fValue[dwIndex] * lpEvent->vecNormal.fValue[dwIndex];
    }

    if (0.99 > dNormal || 1.01 < dNormal || !isfinite(lpEvent->fOverbounce) ||
        0.0F >= lpEvent->vecNormal.fValue[2] || 0.7F <= lpEvent->vecNormal.fValue[2]) {
        return FALSE;
    }

    lpMetrics->dIncoming = GetLength(&lpEvent->vecIncoming);
    lpMetrics->dOutgoing = GetLength(&lpEvent->vecOutgoing);

    if (lpMetrics->dIncoming <= 0.000001 || dDot > 0.001) {
        return FALSE;
    }

    dCos = fabs(dDot) / (lpMetrics->dIncoming * dNormal);
    if (dCos > 1.0) {
        dCos = 1.0;
    }

    lpMetrics->dAngle = acos(dCos) * 180.0 / 3.14159265358979323846;
    lpMetrics->dDelta = lpMetrics->dOutgoing - lpMetrics->dIncoming;
    lpMetrics->dRetention = 100.0 * lpMetrics->dOutgoing / lpMetrics->dIncoming;
    lpMetrics->dHorizontalDelta = hypot(lpEvent->vecOutgoing.fValue[0], lpEvent->vecOutgoing.fValue[1]) -
        hypot(lpEvent->vecIncoming.fValue[0], lpEvent->vecIncoming.fValue[1]);

    return TRUE;
}

VOID ForgetResult(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ ULONG_PTR ulResult
) {
    for (DWORD dwIndex = 0; dwIndex < SERVER_BOARD_PENDING_CAPACITY; ++dwIndex) {
        if (ulResult == lpPending[dwIndex].Event.ulResult) {
            lpPending[dwIndex].bValid = FALSE;
        }
    }
}

BOOL StoreCandidate(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ CONST LPBOARD_EVENT lpEvent
) {
    if (NULL == lpPending || NULL == lpEvent) {
        return FALSE;
    }

    ForgetResult(lpPending, lpEvent->ulResult);
    
    for (DWORD dwIndex = 0; dwIndex < SERVER_BOARD_PENDING_CAPACITY; ++dwIndex) {
        if (lpEvent->iTick != lpPending[dwIndex].Event.iTick) {
            lpPending[dwIndex].bValid = FALSE;
        }
    }

    for (DWORD dwIndex = 0; dwIndex < SERVER_BOARD_PENDING_CAPACITY; ++dwIndex) {
        if (!lpPending[dwIndex].bValid) {
            lpPending[dwIndex].Event = *lpEvent;
            lpPending[dwIndex].bValid = TRUE;

            return TRUE;
        }
    }

    return FALSE;
}

BOOL TakeCandidate(
    _Inout_ LPBOARD_PENDING lpPending,
    _In_ ULONG_PTR ulResult,
    _In_ ULONG_PTR ulPlayer,
    _In_ INT iTick,
    _In_ CONST BOARD_VECTOR *lpIncoming,
    _Out_ LPBOARD_EVENT lpEvent
) {
    if (NULL == lpEvent) {
        return FALSE;
    }

    ZeroMemory(lpEvent, sizeof(BOARD_EVENT));

    for (DWORD dwIndex = 0; dwIndex < SERVER_BOARD_PENDING_CAPACITY; ++dwIndex) {
        LPBOARD_PENDING lpSlot = &lpPending[dwIndex];

        if (lpSlot->bValid && ulResult == lpSlot->Event.ulResult) {
            lpSlot->bValid = FALSE;
            
            if (ulPlayer != lpSlot->Event.ulPlayer) {
                return FALSE;
            } 
            if (iTick != lpSlot->Event.iTick) {
                return FALSE;
            }
            
            if (0 != memcmp(lpIncoming, &lpSlot->Event.vecIncoming, sizeof(*lpIncoming))) {
                return FALSE;
            }
            
            *lpEvent = lpSlot->Event;

            return TRUE;
        }
    }

    return FALSE;
}
