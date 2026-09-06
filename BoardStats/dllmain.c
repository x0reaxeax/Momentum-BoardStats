// DX11 in-frame HUD. Rendering is isolated from movement hooks and restores context state.
#define COBJMACROS
#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "Core.h"
#include "Components.h"
#include "Presentation.h"
#include "MinHook.h"

#define DROP_COM(p) \
    do { \
        if ((p) != NULL) { \
            IUnknown_Release((IUnknown *)(p)); \
            (p) = NULL; \
        } \
    } while (0)

typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);
typedef BOOL(WINAPI *Snapshot_t)(LPHUD_STATE);

typedef struct _HOOK_CONTEXT {
    Present_t lpPresent;
    LPVOID lpTarget;
    Snapshot_t lpSnapshot;
    BOOL bInitialized;
    BOOL bFailed;
} HOOK_CONTEXT;

typedef struct _RUNTIME_CONTEXT {
    HMODULE hSelf;
    SRWLOCK ApiLock;
    volatile LONG lEnabled;
    volatile LONG lHidden;
    volatile LONG lFrames;
    volatile LONG lError;
    HANDLE hStop;
    HANDLE hWorker;
    HANDLE hReady;
} RUNTIME_CONTEXT;

typedef struct _HUD_CONTEXT {
    SRWLOCK RenderLock;
    HUD_STATE State;
    HUD_OPTIONS Options;
    HUD_PANEL Panel;
    WCHAR szConfig[32768];
    ULONGLONG qwSampled;
    ULONGLONG qwBoardTime;
    UINT64 qwSequence;
    DWORD dwPanelThread;
} HUD_CONTEXT;

typedef struct _GRAPHICS_CONTEXT {
    ID3DBlob *lpVsCode;
    ID3DBlob *lpPsCode;
    ID3D11Device *lpDevice;
    ID3D11DeviceContext1 *lpContext;
    ID3DDeviceContextState *lpHudContext;
    ID3D11VertexShader *lpVs;
    ID3D11PixelShader *lpPs;
    ID3D11Texture2D *lpTexture;
    ID3D11ShaderResourceView *lpView;
    ID3D11SamplerState *lpSampler;
    ID3D11RasterizerState *lpRaster;
    ID3D11BlendState *lpBlend;
} GRAPHICS_CONTEXT;

static HOOK_CONTEXT g_Hook = { 0 };
static RUNTIME_CONTEXT g_Runtime = { .ApiLock = SRWLOCK_INIT, .lError = S_OK };
static HUD_CONTEXT g_Hud = { .RenderLock = SRWLOCK_INIT };
static GRAPHICS_CONTEXT g_Graphics = { 0 };

static VOID ReleaseGraphics(
    VOID
) {
    DROP_COM(g_Graphics.lpHudContext);
    DROP_COM(g_Graphics.lpRaster);
    DROP_COM(g_Graphics.lpBlend);
    DROP_COM(g_Graphics.lpSampler);
    DROP_COM(g_Graphics.lpView);
    DROP_COM(g_Graphics.lpTexture);
    DROP_COM(g_Graphics.lpVs);
    DROP_COM(g_Graphics.lpPs);
    DROP_COM(g_Graphics.lpContext);
    DROP_COM(g_Graphics.lpDevice);
}

static HRESULT CreateGraphics(
    _In_ ID3D11Device *lpDevice
) {
    ID3D11Device1 *lpDevice1 = NULL;
    ID3D11DeviceContext *lpContext = NULL;

    D3D11_SAMPLER_DESC samplerDesc = { 0 };
    
    D3D11_BLEND_DESC blendDesc = {
        .RenderTarget[0].BlendEnable = TRUE,
        .RenderTarget[0].SrcBlend = D3D11_BLEND_ONE,
        .RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA,
        .RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD,
        .RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE,
        .RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA,
        .RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD,
        .RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL
    };

    D3D11_RASTERIZER_DESC rasterDesc = { 
        .FillMode = D3D11_FILL_SOLID,
        .CullMode = D3D11_CULL_NONE,
        .ScissorEnable = TRUE,
        .DepthClipEnable = TRUE
    };

    D3D11_TEXTURE2D_DESC textureDesc = {
        .Width = PANEL_WIDTH,
        .Height = PANEL_HEIGHT,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
    };

    D3D_FEATURE_LEVEL featLevel = ID3D11Device_GetFeatureLevel(lpDevice);
    HRESULT hr = ID3D11Device_QueryInterface(
        lpDevice, 
        &IID_ID3D11Device1, 
        (LPVOID *) &lpDevice1
    );
    
    if (FAILED(hr)) {
        return hr;
    }

    ID3D11Device_GetImmediateContext(lpDevice, &lpContext);
    hr = ID3D11DeviceContext_QueryInterface(
        lpContext, 
        &IID_ID3D11DeviceContext1, 
        (LPVOID *) &g_Graphics.lpContext
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device1_CreateDeviceContextState(
        lpDevice1,
        (ID3D11Device_GetCreationFlags(lpDevice) & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
        D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0,
        &featLevel, 
        1, 
        D3D11_SDK_VERSION, 
        &IID_ID3D11Device, 
        NULL, 
        &g_Graphics.lpHudContext
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device_CreateVertexShader(
        lpDevice, 
        ID3D10Blob_GetBufferPointer(g_Graphics.lpVsCode),
        ID3D10Blob_GetBufferSize(g_Graphics.lpVsCode),
        NULL, 
        &g_Graphics.lpVs
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device_CreatePixelShader(
        lpDevice, 
        ID3D10Blob_GetBufferPointer(g_Graphics.lpPsCode),
        ID3D10Blob_GetBufferSize(g_Graphics.lpPsCode),
        NULL, 
        &g_Graphics.lpPs
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }
    
    hr = ID3D11Device_CreateTexture2D(
        lpDevice, 
        &textureDesc, 
        NULL, 
        &g_Graphics.lpTexture
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device_CreateShaderResourceView(
        lpDevice, 
        (ID3D11Resource *) g_Graphics.lpTexture, 
        NULL, 
        &g_Graphics.lpView
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    // avoid sampling outside the texture bounds
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(
        lpDevice, 
        &samplerDesc, 
        &g_Graphics.lpSampler
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device_CreateBlendState(lpDevice, &blendDesc, &g_Graphics.lpBlend);
    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = ID3D11Device_CreateRasterizerState(
        lpDevice, 
        &rasterDesc, 
        &g_Graphics.lpRaster
    );

    if (SUCCEEDED(hr)) {
        g_Graphics.lpDevice = lpDevice;
        ID3D11Device_AddRef(lpDevice);
        g_Hud.qwSampled = 0;
    }

_FINAL:
    DROP_COM(lpContext);
    DROP_COM(lpDevice1);
    if (FAILED(hr)) {
        ReleaseGraphics();
    }
    return hr;
}

static HRESULT DrawFrame(
    _In_ IDXGISwapChain *lpSwapChain
) {
    ID3D11Device *lpDevice = NULL;
    ID3D11Texture2D *lpBackbuffer = NULL;
    ID3D11RenderTargetView *lpTarget = NULL;
    ID3DDeviceContextState *lpPrevious = NULL;

    D3D11_TEXTURE2D_DESC backText = { 0 };
    D3D11_MAPPED_SUBRESOURCE mapSubres = { 0 };
    DXGI_SWAP_CHAIN_DESC chainDesc = { 0 };
    D3D11_VIEWPORT viewport = { 0 };
    RECT rectBounds = { 0 };
    DWORD dwPid = 0;
    
    HRESULT hr = IDXGISwapChain_GetDesc(lpSwapChain, &chainDesc);
    ULONGLONG qwNow = GetTickCount64();

    if (FAILED(hr) || NULL == chainDesc.OutputWindow) {
        return hr;
    }

    GetWindowThreadProcessId(chainDesc.OutputWindow, &dwPid);
    if (dwPid != GetCurrentProcessId() || !IsWindowVisible(chainDesc.OutputWindow)) {
        return S_FALSE;
    }

    hr = IDXGISwapChain_GetDevice(
        lpSwapChain, 
        &IID_ID3D11Device, 
        (LPVOID *) &lpDevice
    );

    if (FAILED(hr)) {
        return hr;
    }

    if (lpDevice != g_Graphics.lpDevice) {
        ReleaseGraphics();
        hr = CreateGraphics(lpDevice);
        
        if (FAILED(hr)) {
            goto _FINAL;
        }
    }

    if (NULL == g_Hud.Panel.hDc || g_Hud.dwPanelThread != GetCurrentThreadId()) {
        DestroyHudPanel(&g_Hud.Panel);
        if (!CreateHudPanel(&g_Hud.Panel)) {
            hr = E_OUTOFMEMORY;
            goto _FINAL;
        }

        g_Hud.dwPanelThread = GetCurrentThreadId();
        g_Hud.qwSampled = 0;
    }

    if (0 == g_Hud.qwSampled || qwNow - g_Hud.qwSampled >= 100) {
        HUD_STATE hudState = { 0 };
        if (g_Hook.lpSnapshot(&hudState) && hudState.dwExpectedPid == GetCurrentProcessId()) {
            g_Hud.State = hudState;
            if (0 == hudState.dwHistoryCount) {
                g_Hud.qwSequence = 0;
                g_Hud.qwBoardTime = 0;
            } else if (hudState.aHistory[0].qwSequence != g_Hud.qwSequence) {
                g_Hud.qwSequence = hudState.aHistory[0].qwSequence;
                g_Hud.qwBoardTime = qwNow;
            }
        }
        
        PaintHudPanel(&g_Hud.Panel, &g_Hud.State, &g_Hud.Options);
        
        hr = ID3D11DeviceContext1_Map(
            g_Graphics.lpContext, 
            (ID3D11Resource *) g_Graphics.lpTexture, 
            0, 
            D3D11_MAP_WRITE_DISCARD, 
            0, 
            &mapSubres
        );

        if (FAILED(hr)) {
            goto _FINAL;
        }

        if (NULL == mapSubres.pData || 0 == mapSubres.RowPitch) {
            ID3D11DeviceContext1_Unmap(
                g_Graphics.lpContext, 
                (ID3D11Resource *) g_Graphics.lpTexture, 
                0
            );

            hr = E_FAIL;
            goto _FINAL;
        }

        for (UINT i = 0; i < PANEL_HEIGHT; ++i) {
            memcpy(
                (BYTE *) mapSubres.pData + i * mapSubres.RowPitch, 
                g_Hud.Panel.lpPixels + i * PANEL_WIDTH, 
                PANEL_WIDTH * sizeof(DWORD)
            );
        }

        ID3D11DeviceContext1_Unmap(g_Graphics.lpContext, (ID3D11Resource *) g_Graphics.lpTexture, 0);
        g_Hud.qwSampled = qwNow;
    }
    
    if (
        (g_Hud.Options.iDuration > 0) && 
        0 != g_Hud.qwBoardTime && 
        (qwNow - g_Hud.qwBoardTime > (ULONGLONG) g_Hud.Options.iDuration)
     ) {
        hr = S_FALSE;
        goto _FINAL;
    }

    hr = IDXGISwapChain_GetBuffer(
        lpSwapChain, 
        0, 
        &IID_ID3D11Texture2D, 
        (LPVOID *) &lpBackbuffer
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    ID3D11Texture2D_GetDesc(lpBackbuffer, &backText);
    hr = ID3D11Device_CreateRenderTargetView(
        lpDevice, 
        (ID3D11Resource *) lpBackbuffer, 
        NULL, 
        &lpTarget
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    PlaceHudPanel(
        &g_Hud.Options, 
        g_Hud.Panel.iHeight, 
        (INT) backText.Width, 
        (INT) backText.Height, 
        &rectBounds
    );

    viewport.TopLeftX = (FLOAT) rectBounds.left;
    viewport.TopLeftY = (FLOAT) rectBounds.top;
    viewport.Width = (FLOAT) (rectBounds.right - rectBounds.left);
    viewport.Height = (FLOAT) (rectBounds.bottom - rectBounds.top) * PANEL_HEIGHT / g_Hud.Panel.iHeight;
    viewport.MaxDepth = 1;

    // restore the entire D3D11 context
    ID3D11DeviceContext1_SwapDeviceContextState(g_Graphics.lpContext, g_Graphics.lpHudContext, &lpPrevious);
    ID3D11DeviceContext1_OMSetRenderTargets(g_Graphics.lpContext, 1, &lpTarget, NULL);
    ID3D11DeviceContext1_RSSetViewports(g_Graphics.lpContext, 1, &viewport);
    ID3D11DeviceContext1_RSSetScissorRects(g_Graphics.lpContext, 1, &rectBounds);
    ID3D11DeviceContext1_RSSetState(g_Graphics.lpContext, g_Graphics.lpRaster);
    ID3D11DeviceContext1_IASetPrimitiveTopology(g_Graphics.lpContext, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext1_OMSetBlendState(g_Graphics.lpContext, g_Graphics.lpBlend, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext1_VSSetShader(g_Graphics.lpContext, g_Graphics.lpVs, NULL, 0);
    ID3D11DeviceContext1_PSSetShader(g_Graphics.lpContext, g_Graphics.lpPs, NULL, 0);
    ID3D11DeviceContext1_PSSetShaderResources(g_Graphics.lpContext, 0, 1, &g_Graphics.lpView);
    ID3D11DeviceContext1_PSSetSamplers(g_Graphics.lpContext, 0, 1, &g_Graphics.lpSampler);
    ID3D11DeviceContext1_Draw(g_Graphics.lpContext, 3, 0);
    ID3D11DeviceContext1_OMSetRenderTargets(g_Graphics.lpContext, 0, NULL, NULL);
    ID3D11DeviceContext1_SwapDeviceContextState(g_Graphics.lpContext, lpPrevious, NULL);
    InterlockedIncrement(&g_Runtime.lFrames);

_FINAL:
    DROP_COM(lpPrevious);
    DROP_COM(lpTarget);
    DROP_COM(lpBackbuffer);
    DROP_COM(lpDevice);
    return hr;
}

static HRESULT STDMETHODCALLTYPE PresentHud(
    IDXGISwapChain *lpSwap,
    UINT dwInterval,
    UINT fFlags
) {
    if (0 == (fFlags & DXGI_PRESENT_TEST) && InterlockedCompareExchange(&g_Runtime.lEnabled, 0, 0) != 0 &&
        0 == InterlockedCompareExchange(&g_Runtime.lHidden, 0, 0) && TryAcquireSRWLockExclusive(&g_Hud.RenderLock)) {
        if (InterlockedCompareExchange(&g_Runtime.lEnabled, 0, 0) != 0) {
            HRESULT hr = DrawFrame(lpSwap);
            if (FAILED(hr)) {
                InterlockedExchange(&g_Runtime.lError, hr);
            }
        }
        ReleaseSRWLockExclusive(&g_Hud.RenderLock);
    }
    return g_Hook.lpPresent(lpSwap, dwInterval, fFlags);
}

static DWORD ReloadOptions(
    VOID
) {
    HUD_OPTIONS hudOptions = { 0 };

    LoadHudOptions(g_Hud.szConfig, &hudOptions);
    
    AcquireSRWLockExclusive(&g_Hud.RenderLock);
    g_Hud.Options = hudOptions;
    g_Hud.qwSampled = 0;
    ReleaseSRWLockExclusive(&g_Hud.RenderLock);
    
    return ERROR_SUCCESS;
}

static DWORD WINAPI HotkeyWorker(
    LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);

    MSG msg = { 0 };
    BOOL bClose = FALSE;

    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);

    bClose = RegisterHotKey(NULL, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H');
    RegisterHotKey(NULL, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');

    if (!bClose) {
        InterlockedExchange(&g_Runtime.lError, HRESULT_FROM_WIN32(ERROR_HOTKEY_ALREADY_REGISTERED));
        SetEvent(g_Runtime.hStop);
    }

    SetEvent(g_Runtime.hReady);
    while (WAIT_TIMEOUT == WaitForSingleObject(g_Runtime.hStop, 25)) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (WM_HOTKEY == msg.message) {
                if (1 == msg.wParam) {
                    InterlockedExchange(
                        &g_Runtime.lHidden,
                        !InterlockedCompareExchange(&g_Runtime.lHidden, 0, 0)
                    );
                } else if (2 == msg.wParam) {
                    SetEvent(g_Runtime.hStop);
                } else if (3 == msg.wParam) {
                    ReloadOptions();
                }
            }
        }
    }

    InterlockedExchange(&g_Runtime.lEnabled, 0);
    if (MH_OK != MH_DisableHook(g_Hook.lpTarget)) {
        g_Hook.bFailed = TRUE;
        InterlockedExchange(&g_Runtime.lError, E_FAIL);
    }

    // wait here until no other thread owns the render lock, then release
    AcquireSRWLockExclusive(&g_Hud.RenderLock);
    ReleaseSRWLockExclusive(&g_Hud.RenderLock);

    CollectorStop(NULL);
    UnregisterHotKey(NULL, 1);
    UnregisterHotKey(NULL, 2);
    UnregisterHotKey(NULL, 3);
    return 0;
}

static DWORD InitializeRenderer(
    VOID
) {
    CONST CHAR cszShader[] =
        "struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD;};"
        "V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);"
        "o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}"
        "Texture2D tex:register(t0);SamplerState sam:register(s0);"
        "float4 ps(V i):SV_TARGET{return tex.Sample(sam,i.uv);}";

    WNDCLASSW wndClass = { 
        .lpfnWndProc = DefWindowProcW,
        .hInstance = g_Runtime.hSelf,
        .lpszClassName = L"SurfDB.BoardStats.Internal.Probe"
    };

    HWND hWindow = NULL;
    IDXGISwapChain *lpSwap = NULL;
    ID3D11Device *lpDevice = NULL;

    DXGI_SWAP_CHAIN_DESC chainDesc = {
        .BufferDesc.Width = 32,
        .BufferDesc.Height = 32,
        .BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc.Count = 1,
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = 1,
        .OutputWindow = hWindow,
        .Windowed = TRUE
    };

    HMODULE hPinned = NULL;
    HRESULT hr = S_OK;
    DWORD dwError = ERROR_GEN_FAILURE;
    
    if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR) g_Runtime.hSelf, 
        &hPinned
    )) {
        return GetLastError();
    }
    
    hr = D3DCompile(
        cszShader, 
        sizeof(cszShader) - 1, 
        NULL, 
        NULL, 
        NULL, 
        "vs", 
        "vs_4_0", 
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, 
        &g_Graphics.lpVsCode, 
        NULL
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    hr = D3DCompile(
        cszShader, 
        sizeof(cszShader) - 1, 
        NULL, 
        NULL, 
        NULL, 
        "ps", 
        "ps_4_0", 
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 
        0, 
        &g_Graphics.lpPsCode, 
        NULL
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    if (0 == RegisterClassW(&wndClass)) {
        dwError = GetLastError();
        goto _FINAL;
    }

    hWindow = CreateWindowExW(
        0, 
        wndClass.lpszClassName, 
        L"", 
        WS_POPUP, 
        0, 0, 32, 32, 
        NULL, NULL,
        g_Runtime.hSelf, 
        NULL
    );

    if (NULL == hWindow) {
        dwError = GetLastError();
        goto _FINAL;
    }   
    
    chainDesc.OutputWindow = hWindow;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL, 
        D3D_DRIVER_TYPE_HARDWARE, 
        NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, 
        &chainDesc, 
        &lpSwap, 
        &lpDevice, 
        NULL, NULL
    );

    if (FAILED(hr)) {
        goto _FINAL;
    }

    g_Hook.lpTarget = (LPVOID) (ULONG_PTR) lpSwap->lpVtbl->Present;
    
    MH_STATUS mhStatus = MH_Initialize();
    if (MH_OK != mhStatus && MH_ERROR_ALREADY_INITIALIZED != mhStatus) {
        goto _FINAL;
    } 

    if (MH_OK != MH_CreateHook(
        g_Hook.lpTarget, 
        (LPVOID) (ULONG_PTR) PresentHud, 
        (LPVOID *) &g_Hook.lpPresent
    )) {
        goto _FINAL;
    }

    if (NULL == g_Hook.lpSnapshot) {
        g_Hook.lpSnapshot = CollectorGetSnapshot;
    }

    dwError = g_Hook.lpSnapshot != NULL ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND;

_FINAL:
    DROP_COM(lpSwap);
    DROP_COM(lpDevice);
    if (NULL != hWindow) {
        DestroyWindow(hWindow);
    }
    
    UnregisterClassW(wndClass.lpszClassName, g_Runtime.hSelf);
    if (FAILED(hr)) {
        InterlockedExchange(&g_Runtime.lError, hr);
        return ERROR_NOT_SUPPORTED;
    }
    return dwError;
}

DWORD WINAPI InternalStart(
    _In_ LPVOID lpConfig
) {
    HANDLE hProcessHeap = GetProcessHeap();
    if (NULL == hProcessHeap) {
        return GetLastError();
    }

    DWORD dwError = ERROR_SUCCESS;
    PWCHAR wszExe = HeapAlloc(
        hProcessHeap, 
        HEAP_ZERO_MEMORY, 
        MAX_PATH_UNICODE * sizeof(WCHAR)
    );

    PWCHAR wszName = NULL;
    if (!TryAcquireSRWLockExclusive(&g_Runtime.ApiLock)) {
        return ERROR_BUSY;
    }

    if (g_Hook.bFailed || (NULL != g_Runtime.hWorker && WAIT_OBJECT_0 != WaitForSingleObject(g_Runtime.hWorker, 0))) {
        dwError = ERROR_ALREADY_EXISTS;
        goto _FINAL;
    }

    GetModuleFileNameW(NULL, wszExe, MAX_PATH_UNICODE);
    wszName = wcsrchr(wszExe, L'\\');
    if (NULL == lpConfig || NULL == wszName || 0 != _wcsicmp(wszName + 1, L"momentum.exe")) {
        dwError = ERROR_BAD_ENVIRONMENT;
        goto _FINAL;
    }

    wcscpy_s(g_Hud.szConfig, ARRAYSIZE(g_Hud.szConfig), lpConfig);
    if (!g_Hook.bInitialized) {
        dwError = InitializeRenderer();
        if (ERROR_SUCCESS != dwError) {
            g_Hook.bFailed = TRUE;
            goto _FINAL;
        }

        g_Hook.bInitialized = TRUE;
    }

    if (g_Runtime.hWorker != NULL) {
        CloseHandle(g_Runtime.hWorker);
    }

    if (g_Runtime.hStop != NULL) {
        CloseHandle(g_Runtime.hStop);
    }

    if (g_Runtime.hReady != NULL) {
        CloseHandle(g_Runtime.hReady);
    }

    g_Runtime.hWorker = NULL;
    g_Runtime.hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_Runtime.hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    
    if (NULL == g_Runtime.hStop || NULL == g_Runtime.hReady) {
        dwError = GetLastError();
        goto _FINAL;
    }

    ReloadOptions();
    InterlockedExchange(&g_Runtime.lHidden, 0);
    InterlockedExchange(&g_Runtime.lError, S_OK);

    if (MH_OK != MH_EnableHook(g_Hook.lpTarget)) {
        dwError = ERROR_CAN_NOT_COMPLETE;
        g_Hook.bFailed = TRUE;
        goto _FINAL;
    }

    InterlockedExchange(&g_Runtime.lEnabled, 1);
    g_Runtime.hWorker = CreateThread(NULL, 0, HotkeyWorker, NULL, 0, NULL);
    if (NULL == g_Runtime.hWorker) {
        dwError = GetLastError();
        InterlockedExchange(&g_Runtime.lEnabled, 0);
        if (MH_OK != MH_DisableHook(g_Hook.lpTarget)) {
            g_Hook.bFailed = TRUE;
        }
        goto _FINAL;
    }
    
    if (
        WAIT_OBJECT_0 != WaitForSingleObject(g_Runtime.hReady, 5000) || 
        WAIT_OBJECT_0 == WaitForSingleObject(g_Runtime.hStop, 0)
    ) {
        SetEvent(g_Runtime.hStop);
        dwError = ERROR_HOTKEY_ALREADY_REGISTERED;
    }

_FINAL:
    if (NULL != wszExe) {
        HeapFree(hProcessHeap, 0, wszExe);
    }

    ReleaseSRWLockExclusive(&g_Runtime.ApiLock);
    return dwError;
}

DWORD WINAPI InternalStop(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
    DWORD dwError = ERROR_SUCCESS;
    
    if (!TryAcquireSRWLockExclusive(&g_Runtime.ApiLock)) {
        return ERROR_BUSY;
    }

    if (NULL == g_Runtime.hWorker) {
        dwError = ERROR_NOT_READY;
    } else {
        SetEvent(g_Runtime.hStop);
        if (WAIT_OBJECT_0 != WaitForSingleObject(g_Runtime.hWorker, 10000)) {
            dwError = ERROR_TIMEOUT;
        } else if (g_Hook.bFailed) {
            dwError = ERROR_CAN_NOT_COMPLETE;
        }
    }

    ReleaseSRWLockExclusive(&g_Runtime.ApiLock);
    return dwError;
}

DWORD WINAPI InternalReload(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
    DWORD dwError = ERROR_NOT_READY;
    
    if (!TryAcquireSRWLockExclusive(&g_Runtime.ApiLock)) {
        return ERROR_BUSY;
    }

    if (g_Hook.bInitialized) {
        dwError = ReloadOptions();
    }

    ReleaseSRWLockExclusive(&g_Runtime.ApiLock);
    return dwError;
}

DWORD WINAPI InternalStatus(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
    return 0 == InterlockedCompareExchange(&g_Runtime.lEnabled, 0, 0) 
        ? 0 
        : (DWORD) InterlockedCompareExchange(&g_Runtime.lFrames, 0, 0) + 1;
}

DWORD WINAPI InternalError(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
    return (DWORD) InterlockedCompareExchange(&g_Runtime.lError, 0, 0);
}

DWORD WINAPI InternalWait(
    _In_ LPVOID lpUnused
) {
    UNREFERENCED_PARAMETER(lpUnused);
    
    HANDLE hWorker = NULL;
    DWORD dwError = ERROR_SUCCESS;
    
    AcquireSRWLockExclusive(&g_Runtime.ApiLock);
    if (NULL == g_Runtime.hWorker) {
        ReleaseSRWLockExclusive(&g_Runtime.ApiLock);
        return dwError;
    }

    BOOL bRet = DuplicateHandle(
        GetCurrentProcess(),
        g_Runtime.hWorker,
        GetCurrentProcess(),
        &hWorker,
        SYNCHRONIZE,
        FALSE,
        0
    );

    dwError = GetLastError();
    ReleaseSRWLockExclusive(&g_Runtime.ApiLock);
    
    if (!bRet) {
        return GetLastError();
    }

    if (hWorker != NULL) {
        if (WAIT_OBJECT_0 != WaitForSingleObject(hWorker, INFINITE)) {
            dwError = GetLastError();
        }
        CloseHandle(hWorker);
    }

    return dwError;
}

BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ DWORD dwReason,
    _In_ LPVOID lpReserved
) {
    UNREFERENCED_PARAMETER(lpReserved);
    if (DLL_PROCESS_ATTACH == dwReason) {
        g_Runtime.hSelf = hInstance;
        CollectorAttach(hInstance);
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}
