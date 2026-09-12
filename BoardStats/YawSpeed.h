#ifndef _BOARDSTATS_YAWSPEED_H
#define _BOARDSTATS_YAWSPEED_H
#include <Windows.h>

/// <summary>
/// Validate and retain the supported client module for read-only sampling.
/// </summary>
/// <param name="bEnabled">Whether to prepare the yaw-speed reader.</param>
/// <returns>No value; unavailable readers report N/A without stopping the HUD.</returns>
VOID ConfigureYawSpeed(
    _In_ BOOL bEnabled
);
/// <summary>
/// Read the supported client's current cl_yawspeed without calling into the game.
/// </summary>
/// <param name="lpValue">Receives a finite value on success, zero on failure.</param>
/// <returns>TRUE if the registered variable has the verified layout.</returns>
BOOL ReadYawSpeed(
    _Out_ FLOAT *lpValue
);
#endif // _BOARDSTATS_YAWSPEED_H
