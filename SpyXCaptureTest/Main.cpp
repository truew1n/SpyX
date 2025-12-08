#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <vector>
#include <string>

// Zaktualizowane nag³ówki
#include "Capture/WindowCapture.h" 

// Linkowanie bibliotek
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// =============================================================
// GLOBALS
// =============================================================
// Abstractions
CD3D11Context    g_D3DContext;
CWindowCapture   g_WindowCapture;

// Application Rendering Resources (The "Viewer" Window)
IDXGISwapChain1 *g_SwapChain = nullptr;
ID3D11RenderTargetView *g_MainRTV = nullptr;
ID3D11VertexShader *g_VS = nullptr;
ID3D11PixelShader *g_PS = nullptr;
ID3D11SamplerState *g_Sampler = nullptr;

// =============================================================
// SHADER SOURCE (Passthrough)
// =============================================================
const char *g_ShaderCode = R"(
Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);

struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

// Generates a full screen triangle from VertexIDs (0, 1, 2)
VS_OUTPUT VS(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PS(VS_OUTPUT input) : SV_TARGET {
    return shaderTexture.Sample(SampleType, input.Tex);
}
)";

// =============================================================
// RENDERING SYSTEM (VIEWER WINDOW)
// =============================================================
void CreateRenderTarget()
{
    if (!g_SwapChain) return;

    ID3D11Texture2D *pBackBuffer = nullptr;
    HRESULT hr = g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&pBackBuffer);
    if (SUCCEEDED(hr))
    {
        g_D3DContext.GetDevice()->CreateRenderTargetView(pBackBuffer, nullptr, &g_MainRTV);
        pBackBuffer->Release();
    }
}

void ResizeSwapChain(HWND hWnd)
{
    if (!g_SwapChain || !g_D3DContext.GetDevice()) return;

    if (g_MainRTV) { g_MainRTV->Release(); g_MainRTV = nullptr; }

    RECT rc; GetClientRect(hWnd, &rc);
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    if (width > 0 && height > 0)
    {
        g_SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();

        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        g_D3DContext.GetContext()->RSSetViewports(1, &vp);
    }
}

HRESULT InitAppRenderer(HWND hWnd)
{
    ID3D11Device *device = g_D3DContext.GetDevice();
    if (!device) return E_FAIL;

    // 1. Create SwapChain for the Viewer Window
    IDXGIFactory2 *factory = nullptr;
    {
        IDXGIDevice *dxgiDevice = nullptr;
        device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice);
        IDXGIAdapter *adapter = nullptr;
        dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void **)&adapter);
        adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
        adapter->Release();
        dxgiDevice->Release();
    }

    RECT rc; GetClientRect(hWnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = rc.right - rc.left;
    scd.Height = rc.bottom - rc.top;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = factory->CreateSwapChainForHwnd(device, hWnd, &scd, nullptr, nullptr, &g_SwapChain);
    factory->Release();
    if (FAILED(hr)) return hr;

    // 2. Setup RTV and Viewport
    CreateRenderTarget();
    ResizeSwapChain(hWnd); // Ensure viewport matches

    // 3. Compile Shaders
    ID3DBlob *vsBlob = nullptr;
    ID3DBlob *psBlob = nullptr;
    D3DCompile(g_ShaderCode, strlen(g_ShaderCode), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_ShaderCode, strlen(g_ShaderCode), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, nullptr);

    if (vsBlob) device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_VS);
    if (psBlob) device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_PS);
    if (vsBlob) vsBlob->Release();
    if (psBlob) psBlob->Release();

    // 4. Create Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    device->CreateSamplerState(&sampDesc, &g_Sampler);

    return S_OK;
}

void RenderFrame()
{
    if (!g_MainRTV || !g_SwapChain) return;

    auto ctx = g_D3DContext.GetContext();

    // Clear Backbuffer
    float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    ctx->ClearRenderTargetView(g_MainRTV, clearColor);

    // Acquire Texture from WindowCapture
    ID3D11Texture2D *capturedTex = nullptr;
    HRESULT hr = g_WindowCapture.AcquireLatestFrame(&capturedTex);

    if (SUCCEEDED(hr) && capturedTex)
    {
        // Create a temporary SRV for the captured frame
        D3D11_TEXTURE2D_DESC desc;
        capturedTex->GetDesc(&desc);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        ID3D11ShaderResourceView *srv = nullptr;
        g_D3DContext.GetDevice()->CreateShaderResourceView(capturedTex, &srvDesc, &srv);

        if (srv)
        {
            // Draw
            ctx->OMSetRenderTargets(1, &g_MainRTV, nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_VS, nullptr, 0);
            ctx->PSSetShader(g_PS, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->PSSetSamplers(0, 1, &g_Sampler);

            ctx->Draw(3, 0); // Fullscreen Triangle

            // Unbind to release lock on texture
            ID3D11ShaderResourceView *nullSRV = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSRV);
            srv->Release();
        }

        capturedTex->Release(); // Important: Release the reference from AcquireLatestFrame
    }

    g_SwapChain->Present(1, 0);
}

// =============================================================
// WINDOW PICKER UI
// =============================================================
struct WindowInfo { HWND hwnd; std::wstring title; };
std::vector<WindowInfo> g_WindowList;
HWND g_hSelectorList = NULL;
HWND g_hSelectedWindow = NULL;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT r; GetWindowRect(hwnd, &r);
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return TRUE;
    wchar_t className[256]; GetClassName(hwnd, className, 256);
    if (wcscmp(className, L"Progman") == 0) return TRUE; // Skip Program Manager
    wchar_t title[256]; GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) > 0) g_WindowList.push_back({ hwnd, title });
    return TRUE;
}

LRESULT CALLBACK SelectorProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_WindowList.clear(); EnumWindows(EnumWindowsProc, 0);
        g_hSelectorList = CreateWindow(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 10, 360, 400, hWnd, (HMENU)101, (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
        CreateWindow(L"BUTTON", L"Select", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 150, 420, 80, 30, hWnd, (HMENU)IDOK, (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
        for (const auto &w : g_WindowList) SendMessage(g_hSelectorList, LB_ADDSTRING, 0, (LPARAM)w.title.c_str());
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK || (LOWORD(wParam) == 101 && HIWORD(wParam) == LBN_DBLCLK)) {
            int idx = (int)SendMessage(g_hSelectorList, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR) { g_hSelectedWindow = g_WindowList[idx].hwnd; DestroyWindow(hWnd); }
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(hWnd); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

HWND DoWindowPicker(HINSTANCE hInst, HWND hParent) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, SelectorProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, L"SelCls", NULL };
    RegisterClassEx(&wc);
    HWND hDlg = CreateWindow(L"SelCls", L"Select Window", WS_VISIBLE | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 100, 100, 400, 500, hParent, NULL, hInst, NULL);

    // Simple modal loop
    EnableWindow(hParent, FALSE);
    MSG msg; while (IsWindow(hDlg) && GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    EnableWindow(hParent, TRUE);

    return g_hSelectedWindow;
}

// =============================================================
// MAIN ENTRY
// =============================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: ResizeSwapChain(hWnd); break;
    case WM_PAINT: RenderFrame(); ValidateRect(hWnd, NULL); break;
    case WM_DESTROY: PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 1. Initialize Core Systems
    if (FAILED(g_D3DContext.Initialize())) {
        MessageBox(NULL, L"Failed to initialize D3D11", L"Error", MB_ICONERROR);
        return -1;
    }

    g_WindowCapture.Initialize(&g_D3DContext);

    // 2. Create Application Window
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, hInst, NULL, NULL, NULL, NULL, L"MainWin", NULL };
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindow(L"MainWin", L"DXCapTest", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, hInst, NULL);

    if (FAILED(InitAppRenderer(hWnd))) return -1;

    // 3. Select Target and Start Capture
    HWND targetHwnd = DoWindowPicker(hInst, hWnd);
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    if (targetHwnd) {
        if (FAILED(g_WindowCapture.StartCapture(targetHwnd))) {
            MessageBox(hWnd, L"Failed to start capture", L"Error", MB_OK);
        }
    }

    // 4. Message Loop
    MSG msg;
    while (true) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            // Continuous render
            RenderFrame();
        }
    }

    // 5. Cleanup
    g_WindowCapture.StopCapture();
    g_D3DContext.Cleanup();

    return 0;
}