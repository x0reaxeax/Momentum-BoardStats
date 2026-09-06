#ifndef _BOARDSTATS_PRESENTATION_H
#define _BOARDSTATS_PRESENTATION_H
#include "HudTypes.h"

#define PANEL_WIDTH 400
#define PANEL_HEIGHT 420

typedef struct _HUD_GRADE {
    double dLoss;
    double dAngle;

    COLORREF color;
} HUD_GRADE;

typedef struct _HUD_OPTIONS {
    INT iScale;
    INT iX;
    INT iY;
    INT iMargin;
    INT iDuration;
    INT iHistory;
    BOOL bLeft;
    BOOL bBottom;
    INT iCompact;
    INT iCenterY;
    BOOL bShowLoss;
    BOOL bChange;
    BOOL bRetention;
    BOOL bAngle;
    BOOL bSpeeds;
    BOOL bHorizontal;
    BOOL bGradeColors;
    BOOL bGradeScale;
    double dGradeSpeed;
    HUD_GRADE aGrades[5];
} HUD_OPTIONS, *LPHUD_OPTIONS;

typedef struct _HUD_PANEL {
    HDC hDc;
    HBITMAP hBitmap;
    HGDIOBJ hOldBitmap;
    DWORD *lpPixels;
    HFONT hSmall;
    HFONT hValue;
    HFONT hBig;
    INT iHeight;
} HUD_PANEL, *LPHUD_PANEL;

/// <summary>
/// Load bounded presentation options from an INI file.
/// </summary>
/// <param name="lpszPath">Absolute INI path; missing keys use defaults.</param>
/// <param name="lpOptions">Receives validated settings.</param>
/// <returns>No value.</returns>
VOID LoadHudOptions(
    _In_ LPCWSTR lpszPath,
    _Out_ LPHUD_OPTIONS lpOptions
);
/// <summary>
/// Create the CPU panel resources.
/// </summary>
/// <param name="lpPanel">Zero-initialized panel.</param>
/// <returns>TRUE on success.</returns>
BOOL CreateHudPanel(
    _Out_ LPHUD_PANEL lpPanel
);
/// <summary>
/// Release panel resources.
/// </summary>
/// <param name="lpPanel">Panel to clear.</param>
/// <returns>No value.</returns>
VOID DestroyHudPanel(
    _Inout_ LPHUD_PANEL lpPanel
);
/// <summary>
/// Rasterize a configurable panel into BGRA pixels.
/// </summary>
/// <param name="lpPanel">Created panel.</param>
/// <param name="lpState">Current measured data.</param>
/// <param name="lpOptions">Presentation settings.</param>
/// <returns>No value.</returns>
VOID PaintHudPanel(
    _Inout_ LPHUD_PANEL lpPanel,
    _In_ CONST HUD_STATE *lpState,
    _In_ CONST HUD_OPTIONS *lpOptions
);
/// <summary>
/// Place a panel within the game backbuffer.
/// </summary>
/// <param name="lpOptions">Position and scale settings.</param>
/// <param name="iHeight">Logical panel height.</param>
/// <param name="iWidth">Backbuffer width.</param>
/// <param name="iScreenHeight">Backbuffer height.</param>
/// <param name="lpRect">Receives clamped pixel bounds.</param>
/// <returns>No value.</returns>
VOID PlaceHudPanel(
    _In_ CONST HUD_OPTIONS *lpOptions,
    _In_ INT iHeight,
    _In_ INT iWidth,
    _In_ INT iScreenHeight,
    _Out_ RECT *lpRect
);
/// <summary>
/// Select the first grade meeting both loss and angle thresholds.
/// </summary>
/// <param name="lpBoard">Measured board; invalid input receives fallback grade.</param>
/// <param name="lpOptions">Configured thresholds and speed scaling.</param>
/// <returns>Grade index 0..4, best to fallback.</returns>
INT GetHudGrade(
    _In_ CONST HUD_BOARD *lpBoard,
    _In_ CONST HUD_OPTIONS *lpOptions
);
#endif
