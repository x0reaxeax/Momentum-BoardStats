#ifndef _BOARDSTATS_COMPONENTS_H
#define _BOARDSTATS_COMPONENTS_H

#include "HudTypes.h"

VOID CollectorAttach(
    _In_ HMODULE hModule
);

BOOL WINAPI CollectorGetSnapshot(
    _Out_ LPHUD_STATE lpState
);

DWORD WINAPI CollectorStart(
    _In_ LPVOID lpUnused
);

DWORD WINAPI CollectorStop(
    _In_ LPVOID lpUnused
);

DWORD WINAPI InternalStart(
    _In_ LPVOID lpConfig
);

DWORD WINAPI InternalStop(
    _In_ LPVOID lpUnused
);

DWORD WINAPI InternalWait(
    _In_ LPVOID lpUnused
);

DWORD WINAPI InternalReload(
    _In_ LPVOID lpUnused
);

DWORD WINAPI InternalStatus(
    _In_ LPVOID lpUnused
);

DWORD WINAPI InternalError(
    _In_ LPVOID lpUnused
);

#endif // !_BOARDSTATS_COMPONENTS_H
