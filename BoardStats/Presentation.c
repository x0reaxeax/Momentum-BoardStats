// Configurable panel rasterization shared by the DX11 renderer and preview tests.
#include <Windows.h>

#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include <stdlib.h>
#include <wctype.h>

#include "Presentation.h"

static INT ReadOption(
    _In_ LPCWSTR lpszPath,
    _In_ LPCWSTR lpszKey,
    _In_ INT iDefault,
    _In_ INT iMin,
    _In_ INT iMax
) {
    INT iValue = (INT) GetPrivateProfileIntW(L"HUD", lpszKey, iDefault, lpszPath);

    return max(iMin, min(iMax, iValue));
}

static double ReadGradeNumber(
    _In_ LPCWSTR lpszPath,
    _In_ LPCWSTR lpszSection,
    _In_ LPCWSTR lpszKey,
    _In_ DOUBLE dDefault,
    _In_ DOUBLE dMin,
    _In_ DOUBLE dMax
) {
    WCHAR wszValue[96] = { 0 };
    WCHAR *lpEnd = NULL;
    DOUBLE dValue = 0;

    GetPrivateProfileStringW(lpszSection, lpszKey, L"", wszValue, ARRAYSIZE(wszValue), lpszPath);
    dValue = wcstod(wszValue, &lpEnd);
    if (lpEnd == wszValue) {
        return dDefault;
    }

    while (iswspace(*lpEnd)) {
        ++lpEnd;
    }

    if (*lpEnd != 0 || !isfinite(dValue) || dValue < dMin || dValue > dMax) {
        return dDefault;
    }

    return dValue;
}

INT GetHudGrade(
    _In_ CONST HUD_BOARD *lpBoard,
    _In_ CONST HUD_OPTIONS *lpOptions
) {
    DOUBLE dScale = 1.0;
    DOUBLE dLoss = 100.0 - lpBoard->dRetention;

    if (
        !isfinite(dLoss) || !isfinite(lpBoard->dIncoming) || 
        lpBoard->dIncoming <= 0 || !isfinite(lpBoard->dAngle) || 
        lpBoard->dAngle < 0 || lpBoard->dAngle > 90
    ) {
        return 4;
    }

    if (lpOptions->bGradeScale) {
        dScale = sqrt(lpBoard->dIncoming / lpOptions->dGradeSpeed);
    }

    for (INT i = 0; i < 4; ++i) {
        if (dLoss <= lpOptions->aGrades[i].dLoss * dScale &&
            lpBoard->dAngle >= lpOptions->aGrades[i].dAngle) {
            return i;
        }
    }

    return 4;
}

VOID LoadHudOptions(
    _In_ LPCWSTR lpszPath,
    _Out_ LPHUD_OPTIONS lpOptions
) {
    WCHAR szCorner[32] = { 0 };

    ZeroMemory(lpOptions, sizeof(*lpOptions));
    lpOptions->iScale = ReadOption(lpszPath, L"Scale", 100, 50, 200);
    lpOptions->iX = ReadOption(lpszPath, L"X", -1, -1, 16384);
    lpOptions->iY = ReadOption(lpszPath, L"Y", -1, -1, 16384);
    lpOptions->iMargin = ReadOption(lpszPath, L"Margin", 24, 0, 1000);
    lpOptions->iDuration = ReadOption(lpszPath, L"DurationMs", 0, 0, 60000);
    lpOptions->iHistory = ReadOption(lpszPath, L"History", 3, 0, 3);
    lpOptions->iCompact = ReadOption(lpszPath, L"Compact", 0, 0, 2);
    lpOptions->iCenterY = ReadOption(lpszPath, L"CenterYOffset", 0, -16384, 16384);
    lpOptions->bShowLoss = ReadOption(lpszPath, L"ShowLoss", 0, 0, 1);
    lpOptions->bChange = ReadOption(lpszPath, L"ShowChange", 1, 0, 1);
    lpOptions->bRetention = ReadOption(lpszPath, L"ShowRetention", 1, 0, 1);
    lpOptions->bAngle = ReadOption(lpszPath, L"ShowAngle", 1, 0, 1);
    lpOptions->bSpeeds = ReadOption(lpszPath, L"ShowSpeeds", 1, 0, 1);
    lpOptions->bHorizontal = ReadOption(lpszPath, L"ShowHorizontal", 0, 0, 1);
    lpOptions->bPaintInNoclip = ReadOption(lpszPath, L"PaintInNoclip", 0, 0, 1);
    lpOptions->bShowYawSpeed = ReadOption(lpszPath, L"ShowYawSpeed", 0, 0, 1);
    lpOptions->iYawX = ReadOption(lpszPath, L"YawX", -1, -1, 16384);
    lpOptions->iYawY = ReadOption(lpszPath, L"YawY", -1, -1, 16384);
    lpOptions->iYawMargin = ReadOption(lpszPath, L"YawMargin", 16, 0, 1000);
    lpOptions->iYawFontSize = ReadOption(lpszPath, L"YawFontSize", 16, 8, 48);

    GetPrivateProfileStringW(
        L"HUD",
        L"YawCorner",
        L"bottom-left",
        szCorner,
        ARRAYSIZE(szCorner),
        lpszPath
    );

    lpOptions->bYawLeft = NULL != wcsstr(szCorner, L"left");
    lpOptions->bYawBottom = NULL != wcsstr(szCorner, L"bottom");
    lpOptions->colorYaw = RGB(230, 230, 230);
    {
        WCHAR szColor[96] = { 0 };
        WCHAR wExtra = 0;
        INT iRed = 0, iGreen = 0, iBlue = 0;

        GetPrivateProfileStringW(L"HUD", L"YawColor", L"", szColor, ARRAYSIZE(szColor), lpszPath);
        if (3 == swscanf_s(szColor, L"%d,%d,%d %lc", &iRed, &iGreen, &iBlue, &wExtra, 1U) &&
            iRed >= 0 && iRed <= 255 && iGreen >= 0 && iGreen <= 255 && iBlue >= 0 && iBlue <= 255) {
            lpOptions->colorYaw = RGB(iRed, iGreen, iBlue);
        }
    }

    lpOptions->bGradeColors = ReadOption(lpszPath, L"GradeColors", 1, 0, 1);
    lpOptions->bGradeScale = ReadOption(lpszPath, L"GradeSpeedScale", 1, 0, 1);
    lpOptions->dGradeSpeed = ReadGradeNumber(lpszPath, L"HUD", L"GradeReferenceSpeed", 1500, 1, 1000000);
    {
        CONST LPCWSTR alSections[5] = {
            L"Grade.Perfect", 
            L"Grade.Good", 
            L"Grade.Okay", 
            L"Grade.Bad", 
            L"Grade.Terrible"
        };

        CONST DOUBLE adLoss[5] = { 0.5, 1.5, 3.0, 5.0, 100.0 };
        CONST DOUBLE adAngle[5] = { 85, 80, 75, 60, 0 };

        CONST COLORREF aColors[5] = {
            RGB(80, 160, 255), 
            RGB(100, 230, 130), 
            RGB(255, 255, 255), 
            RGB(255, 220, 80), 
            RGB(255, 90, 90)
        };

        for (INT i = 0; i < 5; ++i) {
            WCHAR szColor[96] = { 0 };
            WCHAR wExtra = 0;
            INT iRed = 0, iGreen = 0, iBlue = 0;

            lpOptions->aGrades[i].dLoss = ReadGradeNumber(
                lpszPath, 
                alSections[i], 
                L"MaxLossPercent", 
                adLoss[i], 
                0, 
                100
            );

            lpOptions->aGrades[i].dAngle = ReadGradeNumber(
                lpszPath, 
                alSections[i], 
                L"MinAngle", 
                adAngle[i], 
                0, 
                90
            );
            
            lpOptions->aGrades[i].color = aColors[i];
            GetPrivateProfileStringW(alSections[i], L"Color", L"", szColor, ARRAYSIZE(szColor), lpszPath);

            if (
                3 == swscanf_s(szColor, L"%d,%d,%d %lc", &iRed, &iGreen, &iBlue, &wExtra, 1U) && 
                iRed >= 0 && iRed <= 255 && iGreen >= 0 && iGreen <= 255 && iBlue >= 0 && iBlue <= 255\
            ) {
                lpOptions->aGrades[i].color = RGB(iRed, iGreen, iBlue);
            }
        }
    }

    GetPrivateProfileStringW(L"HUD", L"Corner", L"top-right", szCorner, ARRAYSIZE(szCorner), lpszPath);
    lpOptions->bLeft = NULL != wcsstr(szCorner, L"left");
    lpOptions->bBottom = NULL != wcsstr(szCorner, L"bottom");
}

BOOL CreateHudPanel(
    _Out_ LPHUD_PANEL lpPanel
) {
    BITMAPINFO info = { 0 };
    lpPanel->hDc = CreateCompatibleDC(NULL);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = PANEL_WIDTH;
    info.bmiHeader.biHeight = -PANEL_HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    lpPanel->hBitmap =
        CreateDIBSection(lpPanel->hDc, &info, DIB_RGB_COLORS, (LPVOID *) &lpPanel->lpPixels, NULL, 0);
    if (NULL == lpPanel->hDc || NULL == lpPanel->hBitmap) {
        DestroyHudPanel(lpPanel);

        return FALSE;
    }
    lpPanel->hOldBitmap = SelectObject(lpPanel->hDc, lpPanel->hBitmap);
    lpPanel->hSmall = CreateFontW(
        -13,
        0, 0, 0,
        FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        0, 0,
        ANTIALIASED_QUALITY,
        0,
        L"Segoe UI"
    );
    lpPanel->hValue = CreateFontW(
        -24,
        0, 0, 0,
        FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        0, 0,
        ANTIALIASED_QUALITY,
        0,
        L"Segoe UI"
    );
    lpPanel->hBig = CreateFontW(
        -48,
        0, 0, 0,
        FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        0, 0,
        ANTIALIASED_QUALITY,
        0,
        L"Segoe UI"
    );

    if (NULL == lpPanel->hSmall || NULL == lpPanel->hValue || NULL == lpPanel->hBig) {
        DestroyHudPanel(lpPanel);

        return FALSE;
    }

    return TRUE;
}

VOID DestroyHudPanel(
    _Inout_ LPHUD_PANEL lpPanel
) {
    if (lpPanel->hOldBitmap != NULL) {
        SelectObject(lpPanel->hDc, lpPanel->hOldBitmap);
    }

    if (lpPanel->hBitmap != NULL) {
        DeleteObject(lpPanel->hBitmap);
    }

    if (lpPanel->hDc != NULL) {
        DeleteDC(lpPanel->hDc);
    }

    DeleteObject(lpPanel->hSmall);
    DeleteObject(lpPanel->hValue);
    DeleteObject(lpPanel->hBig);
    ZeroMemory(lpPanel, sizeof(*lpPanel));
}

static VOID TextLine(
    _Inout_ LPHUD_PANEL lpPanel,
    _In_ HFONT hFont,
    _In_ COLORREF color,
    _In_ INT iY,
    _In_ INT iHeight,
    _In_ LPCWSTR lpszText
) {
    RECT rect = {24, iY, PANEL_WIDTH - 24, iY + iHeight};
    HGDIOBJ hOld = SelectObject(lpPanel->hDc, hFont);

    SetTextColor(lpPanel->hDc, color);
    DrawTextW(lpPanel->hDc, lpszText, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(lpPanel->hDc, hOld);
}

static VOID PaintMinimal(
    _Inout_ LPHUD_PANEL lpPanel,
    _In_ CONST HUD_STATE *lpState,
    _In_ CONST HUD_OPTIONS *lpOptions
) {
    WCHAR wszLine[192] = { 0 };

    COLORREF colorRef = RGB(255, 255, 255);
    RECT rect = {0, 0, PANEL_WIDTH, 32};
    HGDIOBJ hOld = SelectObject(lpPanel->hDc, lpPanel->hValue);
    CONST HUD_BOARD *lpBoard = &lpState->aHistory[0];

    ZeroMemory(lpPanel->lpPixels, PANEL_WIDTH * PANEL_HEIGHT * sizeof(DWORD));
    SetBkMode(lpPanel->hDc, TRANSPARENT);
    SetTextColor(lpPanel->hDc, RGB(255, 255, 255));
    if (lpState->dwHistoryCount != 0) {
        swprintf_s(wszLine, ARRAYSIZE(wszLine), L"Speed Change: %+.2f u/s", lpBoard->dDelta);
        DrawTextW(lpPanel->hDc, wszLine, -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        rect.top = 32;
        rect.bottom = 64;
        swprintf_s(
            wszLine,
            ARRAYSIZE(wszLine),
            L"Speed %s: %.3f%%",
            lpOptions->bShowLoss ? L"Lost" : L"Retained",
            lpOptions->bShowLoss ? 100.0 - lpBoard->dRetention : lpBoard->dRetention
        );
        DrawTextW(lpPanel->hDc, wszLine, -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(lpPanel->hDc, hOld);
    GdiFlush();
    if (lpOptions->bGradeColors) {
        colorRef = lpOptions->aGrades[GetHudGrade(lpBoard, lpOptions)].color;
    }
    // Rasterize white coverage first, then tint without changing glyph opacity.
    for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
        DWORD dwPixel = lpPanel->lpPixels[i];
        DWORD dwCoverage = max(dwPixel & 255, max((dwPixel >> 8) & 255, (dwPixel >> 16) & 255));

        lpPanel->lpPixels[i] = (dwCoverage << 24) | ((GetRValue(colorRef) * dwCoverage / 255) << 16) |
                               ((GetGValue(colorRef) * dwCoverage / 255) << 8) |
                               (GetBValue(colorRef) * dwCoverage / 255);
    }
    lpPanel->iHeight = 64;
}

VOID PaintHudPanel(
    _Inout_ LPHUD_PANEL lpPanel,
    _In_ CONST HUD_STATE *lpState,
    _In_ CONST HUD_OPTIONS *lpOptions
) {
    CONST COLORREF colorText = RGB(235, 242, 249);
    CONST COLORREF colorMuted = RGB(139, 157, 177);
    CONST COLORREF colorAccent = RGB(93, 226, 210);
    WCHAR wszLine[192] = { 0 };
    CONST HUD_BOARD *lpBoard = &lpState->aHistory[0];

    COLORREF colorGrade = lpOptions->aGrades[GetHudGrade(lpBoard, lpOptions)].color;
    INT iY = 48;
    BOOL bBoard = lpState->dwHistoryCount != 0;
    LPCWSTR lpszStatus =
        lpState->bStopped
            ? L"STOPPED"
            : (
                lpState->qwHeartbeat != 0 && 
                GetTickCount64() - lpState->qwHeartbeat < 3000 
                ? L"LIVE"
                : L"WAITING"
            );

    if (2 == lpOptions->iCompact) {
        PaintMinimal(lpPanel, lpState, lpOptions);

        return;
    }

    for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
        lpPanel->lpPixels[i] = 0x000D151F;
    }
    SetBkMode(lpPanel->hDc, TRANSPARENT);
    swprintf_s(wszLine, ARRAYSIZE(wszLine), L"BOARD STATS                            %s", lpszStatus);
    TextLine(lpPanel, lpPanel->hSmall, colorText, 14, 22, wszLine);
    if (!bBoard) {
        TextLine(lpPanel, lpPanel->hValue, colorMuted, iY, 38, L"Board a ramp to begin");
        iY += 44;
    } else {
        if (lpOptions->bChange) {
            TextLine(lpPanel, lpPanel->hSmall, colorMuted, iY, 20, L"3D SPEED CHANGE");
            iY += 20;
            swprintf_s(wszLine, ARRAYSIZE(wszLine), L"%+.2f units/s", lpBoard->dDelta);
            TextLine(
                lpPanel,
                lpOptions->iCompact ? lpPanel->hValue : lpPanel->hBig,
                lpOptions->bGradeColors ? colorGrade : (lpBoard->dDelta < 0 ? RGB(255, 189, 133) : colorAccent),
                iY,
                lpOptions->iCompact ? 32 : 60,
                wszLine
            );
            iY += lpOptions->iCompact ? 36 : 64;
        }

        if (lpOptions->bRetention) {
            swprintf_s(
                wszLine,
                ARRAYSIZE(wszLine),
                L"%.3f%%  %s",
                lpOptions->bShowLoss ? 100.0 - lpBoard->dRetention : lpBoard->dRetention,
                lpOptions->bShowLoss ? L"lost" : L"retained"
            );
            TextLine(
                lpPanel,
                lpOptions->iCompact ? lpPanel->hSmall : lpPanel->hValue,
                lpOptions->bGradeColors ? colorGrade : colorAccent,
                iY,
                30,
                wszLine
            );
            iY += 32;
        }

        if (lpOptions->bAngle) {
            swprintf_s(
                wszLine,
                ARRAYSIZE(wszLine),
                L"%.3f\x00B0  approach / 90\x00B0 = grazing",
                lpBoard->dAngle
            );
            TextLine(lpPanel, lpPanel->hSmall, colorText, iY, 24, wszLine);
            iY += 26;
        }

        if (lpOptions->bSpeeds) {
            swprintf_s(
                wszLine,
                ARRAYSIZE(wszLine),
                L"#%llu   %.2f \x2192 %.2f units/s",
                lpBoard->qwSequence,
                lpBoard->dIncoming,
                lpBoard->dOutgoing
            );
            TextLine(lpPanel, lpPanel->hSmall, colorMuted, iY, 24, wszLine);
            iY += 26;
        }

        if (lpOptions->bHorizontal) {
            swprintf_s(wszLine, ARRAYSIZE(wszLine), L"Horizontal change: %+.2f units/s", lpBoard->dHorizontal);
            TextLine(lpPanel, lpPanel->hSmall, colorText, iY, 24, wszLine);
            iY += 26;
        }

        if (lpOptions->iHistory > 0) {
            TextLine(lpPanel, lpPanel->hSmall, colorMuted, iY + 4, 24, L"PREVIOUS BOARDS");
            iY += 30;
            for (INT i = 1; i <= lpOptions->iHistory && (DWORD) i < lpState->dwHistoryCount; ++i) {
                swprintf_s(
                    wszLine,
                    ARRAYSIZE(wszLine),
                    L"#%llu       %+.2f u/s       %.3f%%",
                    lpState->aHistory[i].qwSequence,
                    lpState->aHistory[i].dDelta,
                    lpState->aHistory[i].dRetention
                );
                TextLine(lpPanel, lpPanel->hSmall, colorText, iY, 22, wszLine);
                iY += 22;
            }
        }
    }
    TextLine(
        lpPanel,
        lpPanel->hSmall,
        colorMuted,
        iY + 4,
        24,
        lpState->dDropped != 0 || lpState->dInvalid != 0 || lpState->dUnmatched != 0 
            ? L"Telemetry gaps detected" 
            : L"Ctrl+Alt+H hide  /  Ctrl+Alt+R reload settings"
    );

    lpPanel->iHeight = min(PANEL_HEIGHT, iY + 38);
    GdiFlush();
    for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
        lpPanel->lpPixels[i] |= 0xFF000000;
    }
}

VOID PlaceHudPanel(
    _In_ CONST HUD_OPTIONS *lpOptions,
    _In_ INT iHeight,
    _In_ INT iWidth,
    _In_ INT iScreenHeight,
    _Out_ RECT *lpRect
) {
    INT iPanelWidth = min(iWidth, MulDiv(PANEL_WIDTH, lpOptions->iScale, 100));
    INT iPanelHeight = min(iScreenHeight, MulDiv(iHeight, lpOptions->iScale, 100));
    INT iX = lpOptions->iX >= 0
                 ? lpOptions->iX
                 : (lpOptions->bLeft ? lpOptions->iMargin : iWidth - iPanelWidth - lpOptions->iMargin);
    INT iY = lpOptions->iY >= 0 ? lpOptions->iY
                                : (lpOptions->bBottom ? iScreenHeight - iPanelHeight - lpOptions->iMargin
                                                      : lpOptions->iMargin);
    if (2 == lpOptions->iCompact) {
        if (lpOptions->iX < 0) {
            iX = (iWidth - iPanelWidth) / 2;
        }

        if (lpOptions->iY < 0) {
            iY = (iScreenHeight - iPanelHeight) / 2 + lpOptions->iCenterY;
        }
    }
    lpRect->left = max(0, min(iWidth - iPanelWidth, iX));
    lpRect->top = max(0, min(iScreenHeight - iPanelHeight, iY));
    lpRect->right = lpRect->left + iPanelWidth;
    lpRect->bottom = lpRect->top + iPanelHeight;
}

VOID PaintYawPanel(
    _Inout_ LPHUD_PANEL lpPanel,
    _In_ CONST HUD_OPTIONS *lpOptions,
    _In_ BOOL bAvailable,
    _In_ FLOAT fValue
) {
    WCHAR szText[64] = { 0 };
    HFONT hFont = CreateFontW(
        -lpOptions->iYawFontSize,
        0, 0, 0,
        FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH,
        L"Segoe UI"
    );
    HGDIOBJ hOld = SelectObject(lpPanel->hDc, NULL != hFont ? hFont : lpPanel->hSmall);
    RECT rect = { 0, 0, PANEL_WIDTH, lpOptions->iYawFontSize + 8 };

    GdiFlush();
    ZeroMemory(lpPanel->lpPixels, PANEL_WIDTH * PANEL_HEIGHT * sizeof(DWORD));
    lpPanel->iHeight = rect.bottom;
    if (bAvailable && isfinite(fValue)) {
        swprintf_s(szText, ARRAYSIZE(szText), L"cl_yawspeed: %.2f", (DOUBLE) fValue);
    } else {
        wcscpy_s(szText, ARRAYSIZE(szText), L"cl_yawspeed: N/A");
    }
    SetBkMode(lpPanel->hDc, TRANSPARENT);
    SetTextColor(lpPanel->hDc, RGB(255, 255, 255));
    
    DrawTextW(
        lpPanel->hDc,
        szText,
        -1,
        &rect,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (lpOptions->bYawLeft ? DT_LEFT : DT_RIGHT)
    );

    SelectObject(lpPanel->hDc, hOld);
    if (NULL != hFont) {
        DeleteObject(hFont);
    }

    GdiFlush();
    for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
        DWORD dwPixel = lpPanel->lpPixels[i];
        DWORD dwCoverage = max(dwPixel & 255U, max((dwPixel >> 8) & 255U, (dwPixel >> 16) & 255U));

        lpPanel->lpPixels[i] = (dwCoverage << 24) |
            ((GetRValue(lpOptions->colorYaw) * dwCoverage / 255U) << 16) |
            ((GetGValue(lpOptions->colorYaw) * dwCoverage / 255U) << 8) |
            (GetBValue(lpOptions->colorYaw) * dwCoverage / 255U);
    }
}
