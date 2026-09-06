#ifndef _BOARDSTATS_HUD_TYPE_H
#define _BOARDSTATS_HUD_TYPE_H

#include <Windows.h>

typedef double DOUBLE;

#define HUD_HISTORY_COUNT 4U

typedef struct _HUD_BOARD {
    UINT64 qwSequence;
    INT iTick;

    DOUBLE dDelta;
    DOUBLE dRetention;
    DOUBLE dAngle;
    DOUBLE dIncoming;
    DOUBLE dOutgoing;
    DOUBLE dHorizontal;
} HUD_BOARD, *LPHUD_BOARD;

typedef struct _HUD_STATE {
    DWORD dwExpectedPid;
    BOOL bSession;
    BOOL bWrongPid;
    BOOL bStopped;
    DWORD dwHistoryCount;
    HUD_BOARD aHistory[HUD_HISTORY_COUNT];
    ULONGLONG qwHeartbeat;
    DWORD dwBadRecords;

    DOUBLE dUnmatched;
    DOUBLE dDropped;
    DOUBLE dInvalid;
    DOUBLE dStopError;
} HUD_STATE, *LPHUD_STATE;

#endif // _BOARDSTATS_HUD_TYPE_H
