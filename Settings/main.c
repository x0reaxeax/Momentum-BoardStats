// Native settings editor. Writes only the adjacent HUD configuration file on Save.
#include <Windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <wchar.h>

#include "Presentation.h"

static WCHAR g_szPath[32768] = { 0 };

static CONST LPCWSTR
    g_alKeys[] = {L"X", L"Y", L"Scale", L"Margin", L"DurationMs", L"History", L"Compact", L"CenterYOffset"};
static CONST LPCWSTR g_alLabels[] = {
    L"X pixels (-1 = automatic)",
    L"Y pixels (-1 = automatic)",
    L"Scale % (50-200)",
    L"Edge margin pixels",
    L"Display ms (0 = persistent)",
    L"Previous boards (0-3)",
    L"Layout (0=full, 1=small, 2=text)",
    L"Text Y offset (-16384..16384)"
};
static CONST INT g_aiMin[] = {-1, -1, 50, 0, 0, 0, 0, -16384};
static CONST INT g_aiMax[] = {16384, 16384, 200, 1000, 60000, 3, 2, 16384};
static CONST LPCWSTR g_alFlags[] = {
    L"ShowLoss",
    L"ShowChange",
    L"ShowRetention",
    L"ShowAngle",
    L"ShowSpeeds",
    L"ShowHorizontal",
    L"GradeColors"
};
static CONST LPCWSTR g_alFlagLabels[] = {
    L"Show percentage lost",
    L"3D speed change",
    L"Speed retained",
    L"Approach angle",
    L"Incoming / outgoing speed",
    L"Horizontal speed change",
    L"Grade colors (edit INI thresholds)"
};
static CONST LPCWSTR g_alCorners[] = {L"top-right", L"top-left", L"bottom-right", L"bottom-left"};

static HWND AddControl(
    _In_ HWND hParent,
    _In_ LPCWSTR lpszClass,
    _In_ LPCWSTR lpszText,
    _In_ DWORD fStyle,
    _In_ INT iX,
    _In_ INT iY,
    _In_ INT iWidth,
    _In_ INT iHeight,
    _In_ INT iId
) {
    HWND hControl = CreateWindowExW(
        0,
        lpszClass,
        lpszText,
        WS_CHILD | WS_VISIBLE | fStyle,
        iX,
        iY,
        iWidth,
        iHeight,
        hParent,
        (HMENU) (INT_PTR) iId,
        GetModuleHandleW(NULL),
        NULL
    );
    SendMessageW(hControl, WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), TRUE);

    return hControl;
}

static BOOL SaveOptions(
    _In_ HWND hWindow
) {
    WCHAR aszValues[8][32] = { 0 };

    LRESULT iCorner = SendDlgItemMessageW(hWindow, 10, CB_GETCURSEL, 0, 0);
    BOOL bSuccess = TRUE;

    for (INT i = 0; i < 8; ++i) {
        WCHAR *lpszEnd = NULL;
        LONG lValue = 0;

        GetDlgItemTextW(hWindow, 20 + i, aszValues[i], ARRAYSIZE(aszValues[i]));
        lValue = wcstol(aszValues[i], &lpszEnd, 10);
        if (0 == aszValues[i][0] || *lpszEnd != 0 || lValue < g_aiMin[i] || lValue > g_aiMax[i]) {
            MessageBoxW(
                hWindow,
                L"Enter a number within the range shown for each setting. X/Y use -1 for automatic " L"placement. Layout 2 centers the text.",
                L"Check settings",
                MB_ICONINFORMATION
            );
            SetFocus(GetDlgItem(hWindow, 20 + i));

            return FALSE;
        }
    }

    if (iCorner < 0 || iCorner > 3) {
        return FALSE;
    }
    bSuccess = WritePrivateProfileStringW(L"HUD", L"Corner", g_alCorners[iCorner], g_szPath);
    for (INT i = 0; i < 8; ++i) {
        bSuccess = WritePrivateProfileStringW(L"HUD", g_alKeys[i], aszValues[i], g_szPath) && bSuccess;
    }

    for (INT i = 0; i < 7; ++i) {
        bSuccess = WritePrivateProfileStringW(
            L"HUD",
            g_alFlags[i],
            BST_CHECKED == SendDlgItemMessageW(hWindow, 40 + i, BM_GETCHECK, 0, 0) ? L"1" : L"0",
            g_szPath
        ) &&
                   bSuccess;
    }
    MessageBoxW(
        hWindow,
        bSuccess ? L"Saved. Press Ctrl+Alt+R in Momentum to apply, or start the internal HUD." : L"Could not save the adjacent boardstats.ini. Check that the folder is writable.",
        L"BoardStats settings",
        bSuccess ? MB_ICONINFORMATION : MB_ICONERROR
    );

    return bSuccess;
}

static LRESULT CALLBACK SettingsProc(
    _In_ HWND hWindow,
    _In_ UINT dwMessage,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam
) {
    switch (dwMessage) {
        case WM_CREATE: {
            HUD_OPTIONS options = { 0 };
            INT aiValues[8] = { 0 };
            BOOL abFlags[7] = { 0 };

            LoadHudOptions(g_szPath, &options);
            aiValues[0] = options.iX;
            aiValues[1] = options.iY;
            aiValues[2] = options.iScale;
            aiValues[3] = options.iMargin;
            aiValues[4] = options.iDuration;
            aiValues[5] = options.iHistory;
            aiValues[6] = options.iCompact;
            aiValues[7] = options.iCenterY;
            abFlags[0] = options.bShowLoss;
            abFlags[1] = options.bChange;
            abFlags[2] = options.bRetention;
            abFlags[3] = options.bAngle;
            abFlags[4] = options.bSpeeds;
            abFlags[5] = options.bHorizontal;
            abFlags[6] = options.bGradeColors;
            AddControl(hWindow, L"STATIC", L"Internal HUD settings", 0, 24, 20, 400, 24, 0);
            AddControl(
                hWindow,
                L"STATIC",
                L"Save, then press Ctrl+Alt+R in Momentum to apply.",
                0,
                24,
                48,
                460,
                24,
                0
            );
            AddControl(hWindow, L"STATIC", L"Corner", 0, 24, 92, 210, 24, 0);
            AddControl(
                hWindow,
                L"COMBOBOX",
                L"",
                CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                240,
                88,
                180,
                180,
                10
            );
            for (INT i = 0; i < 4; ++i) {
                SendDlgItemMessageW(hWindow, 10, CB_ADDSTRING, 0, (LPARAM) g_alCorners[i]);
            }
            SendDlgItemMessageW(
                hWindow,
                10,
                CB_SETCURSEL,
                (options.bBottom ? 2 : 0) + (options.bLeft ? 1 : 0),
                0
            );
            for (INT i = 0; i < 8; ++i) {
                AddControl(hWindow, L"STATIC", g_alLabels[i], 0, 24, 128 + 34 * i, 210, 24, 0);
                AddControl(
                    hWindow,
                    L"EDIT",
                    L"",
                    WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                    240,
                    124 + 34 * i,
                    180,
                    25,
                    20 + i
                );
                SetDlgItemInt(hWindow, 20 + i, aiValues[i], TRUE);
            }

            for (INT i = 0; i < 7; ++i) {
                AddControl(
                    hWindow,
                    L"BUTTON",
                    g_alFlagLabels[i],
                    BS_AUTOCHECKBOX | WS_TABSTOP,
                    450,
                    90 + 36 * i,
                    220,
                    28,
                    40 + i
                );
                SendDlgItemMessageW(
                    hWindow,
                    40 + i,
                    BM_SETCHECK,
                    abFlags[i] ? BST_CHECKED : BST_UNCHECKED,
                    0
                );
            }
            AddControl(
                hWindow,
                L"STATIC",
                L"Coordinates are relative to the game frame, not your desktop.",
                0,
                24,
                408,
                640,
                24,
                0
            );
            AddControl(
                hWindow,
                L"BUTTON",
                L"Advanced config",
                BS_PUSHBUTTON | WS_TABSTOP,
                24,
                454,
                180,
                32,
                100
            );
            AddControl(hWindow, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 450, 454, 100, 32, IDOK);
            AddControl(hWindow, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 570, 454, 100, 32, IDCANCEL);

            return 0;
        }
        case WM_COMMAND:
            if (100 == LOWORD(wParam)) {
                WCHAR wszArgument[32772] = { 0 };

                swprintf_s(wszArgument, ARRAYSIZE(wszArgument), L"\"%s\"", g_szPath);
                if ((INT_PTR)
                        ShellExecuteW(hWindow, L"open", L"notepad.exe", wszArgument, NULL, SW_SHOWNORMAL) <=
                    32) {
                    MessageBoxW(hWindow, L"Could not open boardstats.ini.", L"BoardStats", MB_ICONERROR);
                }
            } else if (IDOK == LOWORD(wParam)) {
                SaveOptions(hWindow);
            } else if (IDCANCEL == LOWORD(wParam)) {
                DestroyWindow(hWindow);
            }

            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);

            return 0;
        default:
            break;
    }

    return DefWindowProcW(hWindow, dwMessage, wParam, lParam);
}

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_ HINSTANCE hPrevious,
    _In_ PWSTR lpszCommand,
    _In_ INT iShow
) {
    WNDCLASSW wc = { 0 };
    HWND hWindow = NULL;

    MSG msg = { 0 };
    WCHAR *lpszName = NULL;
    UNREFERENCED_PARAMETER(hPrevious);
    UNREFERENCED_PARAMETER(lpszCommand);
    UNREFERENCED_PARAMETER(iShow);

    GetModuleFileNameW(NULL, g_szPath, ARRAYSIZE(g_szPath));
    lpszName = wcsrchr(g_szPath, L'\\');
    if (NULL == lpszName) {
        return 1;
    }
    lpszName[1] = 0;
    wcscat_s(g_szPath, ARRAYSIZE(g_szPath), L"boardstats.ini");
    wc.hInstance = hInstance;
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"SurfDB.Internal.Settings";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) {
        return 1;
    }
    hWindow = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        wc.lpszClassName,
        L"BoardStats internal HUD settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        720,
        550,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    if (NULL == hWindow) {
        return 1;
    }
    ShowWindow(hWindow, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hWindow, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return 0;
}
