// Real DX11 render/readback and resize checks on a private offscreen test window.
#include "../BoardStats/dllmain.c"
#include <stdlib.h>
#include "../BoardStats/Paint.c"
#include "../BoardStats/YawSpeed.c"

#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x);                                           \
            ExitProcess(1);                                                                                  \
        }                                                                                                    \
    } while (0)

static VOID TestYawReader(
    VOID
) {
    BYTE *lpBase = VirtualAlloc(NULL, 0x1120000U, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BYTE *lpVariable = NULL;
    FLOAT fValue = 0;
    DWORD dwBits = 0;

    CHECK(NULL != lpBase);
    lpVariable = lpBase + CLIENT_YAWSPEED_RVA;
    *(BYTE **) lpVariable = lpBase + CLIENT_CONVAR_VTABLE_RVA;
    *(BYTE **) (lpVariable + CONVAR_PARENT_OFFSET) = lpVariable;
    g_hYawClient = (HMODULE) lpBase;
    g_bYawReady = TRUE;
    for (INT i = 0; i < 2; ++i) {
        FLOAT fExpected = 0 == i ? 97.0F : 210.25F;

        memcpy(&dwBits, &fExpected, sizeof(dwBits));
        *(DWORD *) (lpVariable + CONVAR_FLOAT_OFFSET) = dwBits ^ (DWORD) (ULONG_PTR) lpVariable;
        CHECK(ReadYawSpeed(&fValue) && fExpected == fValue);
    }
    *(DWORD *) (lpVariable + CONVAR_FLOAT_OFFSET) = 0x7FC00000U ^ (DWORD) (ULONG_PTR) lpVariable;
    CHECK(!ReadYawSpeed(&fValue) && 0 == fValue);
    *(BYTE **) (lpVariable + CONVAR_PARENT_OFFSET) = NULL;
    CHECK(!ReadYawSpeed(&fValue));
    g_bYawReady = FALSE;
    g_hYawClient = NULL;
    CHECK(!ReadYawSpeed(&fValue));
    CHECK(!ReadYawSpeed(NULL));
    VirtualFree(lpBase, 0, MEM_RELEASE);
}

static UINT g_uPaintFixtureCalls = 0;
static VOID __fastcall CheckPaintArguments(
    _In_ LPVOID lpPlayer,
    _In_ CONST BOARD_VECTOR *lpOrigin,
    _In_ CONST BOARD_VECTOR *lpAngles,
    _In_ UINT uType,
    _In_ BYTE bMode,
    _In_ INT iSeed,
    _In_ FLOAT fSpread
) {
    CHECK((LPVOID) (ULONG_PTR) 0x1234U == lpPlayer);
    CHECK(10.0F == lpOrigin->fValue[0] && 30.0F == lpOrigin->fValue[2]);
    CHECK(40.0F == lpAngles->fValue[0] && 60.0F == lpAngles->fValue[2]);
    CHECK(5U == uType && 7U == bMode && -123 == iSeed);
    CHECK(0.375F == fSpread);
    ++g_uPaintFixtureCalls;
}

static VOID TestPaintForwarding(
    VOID
) {
    BOARD_VECTOR vecOrigin = { { 10.0F, 20.0F, 30.0F } };
    BOARD_VECTOR vecAngles = { { 40.0F, 50.0F, 60.0F } };
    EmitPaint_t lpPrevious = g_lpEmitPaint;

    g_lpEmitPaint = CheckPaintArguments;
    EmitCameraPaint((LPVOID) (ULONG_PTR) 0x1234U, &vecOrigin, &vecAngles, 5U, 7U, -123, 0.375F);
    InterlockedExchange(&g_lPaintEnabled, 1);
    EmitCameraPaint((LPVOID) (ULONG_PTR) 0x1234U, &vecOrigin, &vecAngles, 5U, 7U, -123, 0.375F);
    InterlockedExchange(&g_lPaintEnabled, 0);
    CHECK(2U == g_uPaintFixtureCalls);
    CHECK(0 == BoardStatsPaintRedirects);
    g_lpEmitPaint = lpPrevious;
}

static HUD_STATE g_TestSample = { 0 };

static BOOL WINAPI ReadTestSnapshot(
    _Out_ LPHUD_STATE lpState
) {
    *lpState = g_TestSample;

    return TRUE;
}

static VOID SavePreview(
    _In_ LPCWSTR lpszPath,
    _In_ CONST D3D11_MAPPED_SUBRESOURCE *lpMapped
) {
    BITMAPFILEHEADER fileHeader = { 0 };
    BITMAPINFOHEADER info = { 0 };
    HANDLE hFile = CreateFileW(lpszPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD cbWritten = 0;

    CHECK(INVALID_HANDLE_VALUE != hFile);
    info.biSize = sizeof(info);
    info.biWidth = 640;
    info.biHeight = -480;
    info.biPlanes = 1;
    info.biBitCount = 32;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
    fileHeader.bfSize = fileHeader.bfOffBits + 640 * 480 * 4;
    CHECK(WriteFile(hFile, &fileHeader, sizeof(fileHeader), &cbWritten, NULL));
    CHECK(WriteFile(hFile, &info, sizeof(info), &cbWritten, NULL));
    for (UINT i = 0; i < 480; ++i) {
        CHECK(WriteFile(hFile, (BYTE *) lpMapped->pData + i * lpMapped->RowPitch, 640 * 4, &cbWritten, NULL));
    }
    CloseHandle(hFile);
}

int wmain(
    _In_ int argc,
    _In_ wchar_t **argv
) {
    HUD_STATE sample = { 0 };

    WNDCLASSW wc = { 0 };
    HWND hWindow = NULL;

    IDXGISwapChain *lpSwap = NULL;
    ID3D11Device *lpDevice = NULL;
    ID3D11DeviceContext *lpContext = NULL;
    ID3D11Texture2D *lpBack = NULL;
    ID3D11Texture2D *lpRead = NULL;
    ID3D11RenderTargetView *lpTarget = NULL;
    ID3D11RenderTargetView *lpRestored = NULL;
    DXGI_SWAP_CHAIN_DESC desc = { 0 };

    D3D11_TEXTURE2D_DESC texture = { 0 };
    D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
    D3D11_VIEWPORT before = {3, 7, 99, 123, 0, 1}, after = { 0 };
    UINT nViewports = 1;
    CONST FLOAT afClear[4] = {0, 0.5F, 0, 1};

    g_Runtime.hSelf = GetModuleHandleW(NULL);
    sample.dwExpectedPid = GetCurrentProcessId();
    sample.bSession = TRUE;
    sample.qwHeartbeat = GetTickCount64();
    sample.dwHistoryCount = 2;
    sample.aHistory[0].qwSequence = 2;
    sample.aHistory[0].dDelta = -40.66;
    sample.aHistory[0].dRetention = 94.936;
    sample.aHistory[0].dAngle = 71.689;
    sample.aHistory[0].dIncoming = 802.96;
    sample.aHistory[0].dOutgoing = 762.30;
    sample.aHistory[1] = sample.aHistory[0];
    sample.aHistory[1].qwSequence = 1;
    g_TestSample = sample;
    g_Hook.lpSnapshot = ReadTestSnapshot;
    CHECK(ERROR_SUCCESS == InitializeRenderer());
    LoadHudOptions(L"Z:\\missing-boardstats-test.ini", &g_Hud.Options);
    {
        HUD_BOARD board = { 0 };

        board.dIncoming = 1500;
        board.dRetention = 99.5;
        board.dAngle = 85;
        CHECK(0 == GetHudGrade(&board, &g_Hud.Options));
        board.dAngle = 84.99;
        CHECK(1 == GetHudGrade(&board, &g_Hud.Options));
        board.dRetention = 98.5;
        board.dAngle = 80;
        CHECK(1 == GetHudGrade(&board, &g_Hud.Options));
        board.dRetention = 97;
        board.dAngle = 75;
        CHECK(2 == GetHudGrade(&board, &g_Hud.Options));
        board.dRetention = 95;
        board.dAngle = 60;
        CHECK(3 == GetHudGrade(&board, &g_Hud.Options));
        board.dAngle = 59.99;
        CHECK(4 == GetHudGrade(&board, &g_Hud.Options));
        board.dIncoming = 6000;
        board.dRetention = 99;
        board.dAngle = 85;
        CHECK(0 == GetHudGrade(&board, &g_Hud.Options));
        g_Hud.Options.bGradeScale = FALSE;
        CHECK(1 == GetHudGrade(&board, &g_Hud.Options));
        g_Hud.Options.bGradeScale = TRUE;
        board.dIncoming = 0;
        CHECK(4 == GetHudGrade(&board, &g_Hud.Options));
    }
    {
        WCHAR szTemp[MAX_PATH] = { 0 };
        WCHAR szIni[MAX_PATH] = { 0 };
        HUD_OPTIONS custom = { 0 };

        CHECK(GetTempPathW(ARRAYSIZE(szTemp), szTemp));
        CHECK(GetTempFileNameW(szTemp, L"bsg", 0, szIni));
        CHECK(WritePrivateProfileStringW(L"Grade.Perfect", L"MaxLossPercent", L"2.25", szIni));
        CHECK(WritePrivateProfileStringW(L"Grade.Perfect", L"Color", L"12,34,56", szIni));
        CHECK(WritePrivateProfileStringW(L"Grade.Good", L"MinAngle", L"nan", szIni));
        CHECK(WritePrivateProfileStringW(L"Grade.Good", L"Color", L"256,0,0", szIni));
        CHECK(WritePrivateProfileStringW(L"HUD", L"GradeReferenceSpeed", L"0", szIni));
        CHECK(WritePrivateProfileStringW(L"HUD", L"ShowYawSpeed", L"1", szIni));
        CHECK(WritePrivateProfileStringW(L"HUD", L"YawCorner", L"top-right", szIni));
        CHECK(WritePrivateProfileStringW(L"HUD", L"YawColor", L"12,34,56", szIni));
        CHECK(WritePrivateProfileStringW(L"HUD", L"YawFontSize", L"100", szIni));
        LoadHudOptions(szIni, &custom);
        CHECK(custom.bShowYawSpeed && !custom.bYawLeft && !custom.bYawBottom);
        CHECK(RGB(12, 34, 56) == custom.colorYaw && 48 == custom.iYawFontSize);
        CHECK(-1 == custom.iYawX && -1 == custom.iYawY && 16 == custom.iYawMargin);
        CHECK(2.25 == custom.aGrades[0].dLoss);
        CHECK(RGB(12, 34, 56) == custom.aGrades[0].color);
        CHECK(80 == custom.aGrades[1].dAngle);
        CHECK(RGB(100, 230, 130) == custom.aGrades[1].color);
        CHECK(1500 == custom.dGradeSpeed);
        CHECK(DeleteFileW(szIni));
    }
    g_Hud.Options.bGradeColors = FALSE;
    g_Hud.Options.bLeft = TRUE;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_Runtime.hSelf;
    wc.lpszClassName = L"SurfDB.Internal.Test";
    CHECK(RegisterClassW(&wc));
    hWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"BoardStats render test",
        WS_POPUP,
        -10000,
        -10000,
        640,
        480,
        NULL,
        NULL,
        g_Runtime.hSelf,
        NULL
    );
    CHECK(hWindow != NULL);
    ShowWindow(hWindow, SW_SHOWNOACTIVATE);
    desc.BufferDesc.Width = 640;
    desc.BufferDesc.Height = 480;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = hWindow;
    desc.Windowed = TRUE;
    CHECK(
        SUCCEEDED(D3D11CreateDeviceAndSwapChain( NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &desc, &lpSwap, &lpDevice, NULL, &lpContext ))
    );
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(lpSwap, 0, &IID_ID3D11Texture2D, (LPVOID *) &lpBack)));
    CHECK(
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(lpDevice, (ID3D11Resource *) lpBack, NULL, &lpTarget))
    );
    ID3D11DeviceContext_OMSetRenderTargets(lpContext, 1, &lpTarget, NULL);
    ID3D11DeviceContext_RSSetViewports(lpContext, 1, &before);
    ID3D11DeviceContext_ClearRenderTargetView(lpContext, lpTarget, afClear);
    CHECK(S_OK == DrawFrame(lpSwap));
    ID3D11DeviceContext_RSGetViewports(lpContext, &nViewports, &after);
    CHECK(0 == memcmp(&before, &after, sizeof(before)));
    ID3D11DeviceContext_OMGetRenderTargets(lpContext, 1, &lpRestored, NULL);
    CHECK(lpRestored == lpTarget);
    DROP_COM(lpRestored);

    ID3D11Texture2D_GetDesc(lpBack, &texture);
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(lpDevice, &texture, NULL, &lpRead)));
    ID3D11DeviceContext_CopyResource(lpContext, (ID3D11Resource *) lpRead, (ID3D11Resource *) lpBack);
    CHECK(
        SUCCEEDED( ID3D11DeviceContext_Map(lpContext, (ID3D11Resource *) lpRead, 0, D3D11_MAP_READ, 0, &mapped) )
    );
    CHECK(0x000D151F == (*(DWORD *) ((BYTE *) mapped.pData + 30 * mapped.RowPitch + 30 * 4) & 0x00FFFFFF));
    if (argc >= 2) {
        SavePreview(argv[1], &mapped);
    }
    ID3D11DeviceContext_Unmap(lpContext, (ID3D11Resource *) lpRead, 0);
    g_Hud.Options.iCompact = 2;
    g_Hud.Options.iCenterY = 40;
    g_Hud.qwSampled = 0;
    ID3D11DeviceContext_ClearRenderTargetView(lpContext, lpTarget, afClear);
    CHECK(S_OK == DrawFrame(lpSwap));
    {
        RECT rect = { 0 };
        UINT nText = 0;

        PlaceHudPanel(&g_Hud.Options, 64, 640, 480, &rect);
        CHECK(120 == rect.left && 248 == rect.top);
        CHECK(0 == g_Hud.Panel.lpPixels[0]);
        ID3D11DeviceContext_CopyResource(lpContext, (ID3D11Resource *) lpRead, (ID3D11Resource *) lpBack);
        CHECK(
            SUCCEEDED( ID3D11DeviceContext_Map(lpContext, (ID3D11Resource *) lpRead, 0, D3D11_MAP_READ, 0, &mapped) )
        );
        CHECK(
            0x00008000 == (*(DWORD *) ((BYTE *) mapped.pData + 248 * mapped.RowPitch + 120 * 4) & 0x00FFFFFF)
        );
        for (INT iY = rect.top; iY < rect.bottom; ++iY) {
            for (INT iX = rect.left; iX < rect.right; ++iX) {
                DWORD dwPixel = *(DWORD *) ((BYTE *) mapped.pData + iY * mapped.RowPitch + iX * 4);

                if ((dwPixel & 255) > 128) {
                    ++nText;
                }
            }
        }
        CHECK(nText > 100);
        if (argc >= 3) {
            SavePreview(argv[2], &mapped);
        }
        ID3D11DeviceContext_Unmap(lpContext, (ID3D11Resource *) lpRead, 0);
        g_Hud.Options.bGradeColors = TRUE;
        g_Hud.qwSampled = 0;
        CHECK(S_OK == DrawFrame(lpSwap));
        {
            BOOL bColored = FALSE;
            DWORD dwExpected = g_Hud.Options.aGrades[GetHudGrade(&sample.aHistory[0], &g_Hud.Options)].color;

            for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
                DWORD dwPixel = g_Hud.Panel.lpPixels[i];

                if (255 == (dwPixel >> 24)) {
                    CHECK(((dwPixel >> 16) & 255) == GetRValue(dwExpected));
                    CHECK((dwPixel & 255) == GetBValue(dwExpected));
                    bColored = TRUE;
                }
                CHECK((dwPixel & 255) <= (dwPixel >> 24));
            }
            CHECK(bColored);
            if (argc >= 4) {
                ID3D11DeviceContext_CopyResource(
                    lpContext,
                    (ID3D11Resource *) lpRead,
                    (ID3D11Resource *) lpBack
                );
                CHECK(
                    SUCCEEDED(ID3D11DeviceContext_Map( lpContext, (ID3D11Resource *) lpRead, 0, D3D11_MAP_READ, 0, &mapped ))
                );
                SavePreview(argv[3], &mapped);
                ID3D11DeviceContext_Unmap(lpContext, (ID3D11Resource *) lpRead, 0);
            }
        }
        g_Hud.Options.bShowLoss = TRUE;
        g_Hud.qwSampled = 0;
        CHECK(S_OK == DrawFrame(lpSwap));
        g_TestSample.dwHistoryCount = 0;
        g_Hud.qwSampled = 0;
        CHECK(S_OK == DrawFrame(lpSwap));
        for (INT i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; ++i) {
            CHECK(0 == g_Hud.Panel.lpPixels[i]);
        }
        g_TestSample = sample;
        g_Hud.Options.iCompact = 0;
        g_Hud.qwSampled = 0;
    }
    {
        UINT nText = 0;
        DWORD dwFirst = 0;
        g_Hud.Options.bShowYawSpeed = TRUE;
        g_Hud.Options.bYawLeft = TRUE;
        g_Hud.Options.bYawBottom = TRUE;
        g_Hud.Options.iYawX = -1;
        g_Hud.Options.iYawY = -1;
        g_Hud.Options.iYawMargin = 16;
        g_Hud.Options.iYawFontSize = 16;
        g_Hud.Options.colorYaw = RGB(230, 230, 230);
        g_Hud.Options.iDuration = 1;
        g_Hud.qwBoardTime = GetTickCount64() - 10;
        g_Hud.qwSampled = GetTickCount64();
        ID3D11DeviceContext_ClearRenderTargetView(lpContext, lpTarget, afClear);
        CHECK(S_OK == DrawFrame(lpSwap));
        ID3D11DeviceContext_RSGetViewports(lpContext, &nViewports, &after);
        CHECK(0 == memcmp(&before, &after, sizeof(before)));
        CHECK(0 == g_Hud.YawPanel.lpPixels[0]);
        ID3D11DeviceContext_CopyResource(lpContext, (ID3D11Resource *) lpRead, (ID3D11Resource *) lpBack);
        CHECK(SUCCEEDED(ID3D11DeviceContext_Map(lpContext, (ID3D11Resource *) lpRead, 0, D3D11_MAP_READ, 0, &mapped)));
        dwFirst = *(DWORD *) mapped.pData;
        for (INT iY = 440; iY < 464; ++iY) {
            for (INT iX = 16; iX < 416; ++iX) {
                DWORD dwPixel = *(DWORD *) ((BYTE *) mapped.pData + iY * mapped.RowPitch + iX * 4);
                nText += dwPixel != dwFirst;
            }
        }
        CHECK(nText > 100 && nText < 5000);
        if (argc >= 5) {
            SavePreview(argv[4], &mapped);
        }
        ID3D11DeviceContext_Unmap(lpContext, (ID3D11Resource *) lpRead, 0);
        // The yaw layer also survives an empty compact board panel.
        g_TestSample.dwHistoryCount = 0;
        g_Hud.Options.iCompact = 2;
        g_Hud.qwSampled = 0;
        CHECK(S_OK == DrawFrame(lpSwap));
        CHECK(0 == g_Hud.Panel.lpPixels[0]);
        g_TestSample = sample;
        g_Hud.Options.iCompact = 0;
        g_Hud.Options.bShowYawSpeed = FALSE;
        g_Hud.Options.iDuration = 0;
        g_Hud.qwSampled = 0;
    }
    ID3D11DeviceContext_OMSetRenderTargets(lpContext, 0, NULL, NULL);
    DROP_COM(lpTarget);
    DROP_COM(lpBack);
    DROP_COM(lpRead);

    CHECK(SUCCEEDED(IDXGISwapChain_ResizeBuffers(lpSwap, 1, 800, 600, DXGI_FORMAT_B8G8R8A8_UNORM, 0)));
    CHECK(S_OK == DrawFrame(lpSwap));
    g_Hud.Options.iDuration = 1;
    g_Hud.qwBoardTime = GetTickCount64() - 10;
    CHECK(S_FALSE == DrawFrame(lpSwap));
    g_Hud.Options.iDuration = 0;
    g_Hud.Options.iX = 16000;
    g_Hud.Options.iY = 16000;
    g_Hud.Options.iScale = 200;
    {
        RECT rect = { 0 };
        PlaceHudPanel(&g_Hud.Options, 300, 640, 480, &rect);
        CHECK(0 == rect.left && 0 == rect.top && 640 == rect.right && 480 == rect.bottom);
    }
    ReleaseGraphics();
    DestroyHudPanel(&g_Hud.Panel);
    DestroyHudPanel(&g_Hud.YawPanel);
    DROP_COM(lpContext);
    DROP_COM(lpDevice);
    DROP_COM(lpSwap);
    DROP_COM(g_Graphics.lpVsCode);
    DROP_COM(g_Graphics.lpPsCode);

    TestYawReader();
    TestPaintForwarding();
    CHECK(PaintShouldOverride(TRUE, 1, TRUE, TRUE));
    CHECK(!PaintShouldOverride(FALSE, 1, TRUE, TRUE));
    CHECK(!PaintShouldOverride(TRUE, 1, FALSE, TRUE));
    CHECK(!PaintShouldOverride(TRUE, 1, TRUE, FALSE));
    for (DWORD dwMode = 0; dwMode < 8; ++dwMode) {
        CHECK((1 == dwMode) == PaintShouldOverride(TRUE, dwMode, TRUE, TRUE));
    }
    CHECK(ERROR_SUCCESS == ConfigurePaint(FALSE));
    CHECK(ERROR_MOD_NOT_FOUND == ConfigurePaint(TRUE));
    CHECK(MH_OK == MH_Uninitialize());
    DestroyWindow(hWindow);
    UnregisterClassW(wc.lpszClassName, g_Runtime.hSelf);
    puts("[+] DX11 pixels, state restoration, ResizeBuffers, expiry, and coordinate clamp passed.");

    return 0;
}
