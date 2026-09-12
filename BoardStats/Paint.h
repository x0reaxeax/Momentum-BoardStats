#ifndef _BOARDSTATS_PAINT_H
#define _BOARDSTATS_PAINT_H

#include <Windows.h>

/// <summary>
/// Enable or disable the build-checked, local free-camera paint ray.
/// </summary>
/// <param name="bEnabled">TRUE enables; FALSE restores ordinary paint behavior.</param>
/// <returns>Win32 status. Unknown builds remain disabled.</returns>
DWORD ConfigurePaint(
    _In_ BOOL bEnabled
);

/// <summary>
/// Limit the exception to the paint caller, local player, and free-camera mode.
/// </summary>
/// <param name="bEnabled">Feature switch.</param>
/// <param name="dwFreeCam">Player m_bInFreeCam flag.</param>
/// <param name="bLocalPlayer">Whether this is the local player.</param>
/// <param name="bPaintCaller">Whether this is the verified paint call site.</param>
/// <returns>TRUE only for the supported paint exception.</returns>
BOOL PaintShouldOverride(
    _In_ BOOL bEnabled,
    _In_ DWORD dwFreeCam,
    _In_ BOOL bLocalPlayer,
    _In_ BOOL bPaintCaller
);

#endif // _BOARDSTATS_PAINT_H
