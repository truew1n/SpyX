#include "WindowOverlay.h"

#include <dxgi1_2.h>
#include <dwmapi.h>
#include <comdef.h>

#pragma comment(lib, "dwmapi.lib")

CWindowOverlay::CWindowOverlay() = default;

CWindowOverlay::~CWindowOverlay()
{
    if (MBlendState) MBlendState->Release();
    if (MRenderTargetView) MRenderTargetView->Release();
    if (MSwapChain) MSwapChain->Release();
    if (MOverlayWindow) DestroyWindow(MOverlayWindow);
    UnregisterClass(L"TAPIOverlayClass", GetModuleHandle(nullptr));
}

bool CWindowOverlay::Initialize(CD3D11Context *Context)
{
    MContext = Context;

    D3D11_BLEND_DESC BlendDesc = {};
    BlendDesc.RenderTarget[0].BlendEnable = TRUE;
    BlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    BlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    BlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    BlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    BlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    BlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    BlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    return SUCCEEDED(MContext->GetDevice()->CreateBlendState(&BlendDesc, &MBlendState));
}

bool CWindowOverlay::BindToWindow(HWND TargetWindow)
{
    MTargetWindow = TargetWindow;
    if (!IsWindow(MTargetWindow))
    {
        return false;
    }

    WNDCLASSEX WindowClass = { sizeof(WNDCLASSEX) };
    WindowClass.lpfnWndProc = WindowProcedure;
    WindowClass.hInstance = GetModuleHandle(nullptr);
    WindowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    WindowClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    WindowClass.lpszClassName = L"TAPIOverlayClass";
    RegisterClassEx(&WindowClass);

    RECT Rect;
    GetWindowRect(MTargetWindow, &Rect);
    int Width = Rect.right - Rect.left;
    int Height = Rect.bottom - Rect.top;
    if (Width < 1) Width = 1;
    if (Height < 1) Height = 1;

    MOverlayWindow = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"TapiOverlayClass", L"Overlay",
        WS_POPUP | WS_VISIBLE,
        Rect.left, Rect.top, Width, Height,
        nullptr, nullptr, WindowClass.hInstance, this
    );

    if (!MOverlayWindow)
    {
        return false;
    }

    SetLayeredWindowAttributes(MOverlayWindow, 0, 0, LWA_COLORKEY);

    MARGINS Margins = { -1 };
    DwmExtendFrameIntoClientArea(MOverlayWindow, &Margins);

    IDXGIFactory2 *Factory = nullptr;
    IDXGIDevice *DxgiDevice = nullptr;
    HRESULT HResult = MContext->GetDevice()->QueryInterface(__uuidof(IDXGIDevice), (void **)&DxgiDevice);
    if (FAILED(HResult))
    {
        return false;
    }

    IDXGIAdapter *Adapter = nullptr;
    DxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void **)&Adapter);
    Adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&Factory);

    DXGI_SWAP_CHAIN_DESC1 SwapChainDesc = {};
    SwapChainDesc.Width = Width;
    SwapChainDesc.Height = Height;
    SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    SwapChainDesc.SampleDesc.Count = 1;
    SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    SwapChainDesc.BufferCount = 1;
    SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    SwapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    SwapChainDesc.Scaling = DXGI_SCALING_STRETCH;

    HResult = Factory->CreateSwapChainForHwnd(
        MContext->GetDevice(),
        MOverlayWindow,
        &SwapChainDesc,
        nullptr, nullptr,
        &MSwapChain
    );

    Factory->Release();
    Adapter->Release();
    DxgiDevice->Release();

    if (FAILED(HResult))
    {
        _com_error ComError(HResult);
        MessageBox(NULL, ComError.ErrorMessage(), L"Swap Chain Error", MB_ICONERROR);
        return false;
    }

    CreateSizeDependentResources(Width, Height);

    MLastRect = Rect;

    UpdateVisiblity();
    UpdatePosition();

    return true;
}

void CWindowOverlay::SetRenderCallback(FRenderDelegate Callback)
{
    MRenderCallback = Callback;
}

void CWindowOverlay::SetWindowProcedureCallback(FWindowProcedureDelegate Callback)
{
    MWindowProcedureCallback = Callback;
}

void CWindowOverlay::SetOverlayVisibility(bool NewOverlayVisibility)
{
    if (NewOverlayVisibility == MOverlayVisibility) return;

    MOverlayVisibility = NewOverlayVisibility;

    UpdateVisiblity();
}

bool CWindowOverlay::GetOverlayVisibility()
{
    return MOverlayVisibility;
}

void CWindowOverlay::Update()
{
    UpdatePosition();
}

void CWindowOverlay::Render()
{
    if (!MRenderTargetView) return;

    if (!IsWindowVisible(MOverlayWindow)) return;

    float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    MContext->GetContext()->ClearRenderTargetView(MRenderTargetView, ClearColor);

    float BlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    MContext->GetContext()->OMSetBlendState(MBlendState, BlendFactor, 0xffffffff);
    MContext->GetContext()->OMSetRenderTargets(1, &MRenderTargetView, nullptr);

    if (MRenderCallback.IsBound())
    {
        MRenderCallback.Execute(MContext->GetContext(), MRenderTargetView);
    }

    MSwapChain->Present(0, 0);
}

bool CWindowOverlay::IsValid() const
{
    return IsWindow(MTargetWindow) && IsWindow(MOverlayWindow);
}

void CWindowOverlay::SetContentProtection(bool Enabled)
{
    if (!MOverlayWindow) return;
    // WDA_EXCLUDEFROMCAPTURE = 0x00000011 (Windows 10 2004+)
    // WDA_MONITOR = 0x00000001 (Windows 7+)
    // We use WDA_EXCLUDEFROMCAPTURE for better protection if available, otherwise system might fallback or fail gracefully.
    // If you are on older windows, try 0x00000001.
    DWORD affinity = Enabled ? 0x00000011 : 0x00000000;
    SetWindowDisplayAffinity(MOverlayWindow, affinity);
}

LRESULT CALLBACK CWindowOverlay::WindowProcedure(HWND WindowHandle, UINT Message, WPARAM WParameter, LPARAM LParameter)
{
    CWindowOverlay *Overlay = (CWindowOverlay *)GetWindowLongPtr(WindowHandle, GWLP_USERDATA);

    if (Message == WM_NCCREATE)
    {
        LPCREATESTRUCT Create = (LPCREATESTRUCT)LParameter;
        Overlay = (CWindowOverlay *)Create->lpCreateParams;
        SetWindowLongPtr(WindowHandle, GWLP_USERDATA, (LONG_PTR)Overlay);
    }

    if (Overlay && Overlay->MWindowProcedureCallback.IsBound())
    {
        LRESULT Result = 0;
        if (Overlay->MWindowProcedureCallback.Execute(WindowHandle, Message, WParameter, LParameter, &Result) == S_OK)
        {
            return Result;
        }
    }

    switch (Message)
    {
        case WM_ERASEBKGND:
        {
            return 1;
        }
    }

    return DefWindowProc(WindowHandle, Message, WParameter, LParameter);
}

void CWindowOverlay::UpdateVisiblity()
{
    if (MOverlayVisibility)
    {
        ShowWindow(MOverlayWindow, SW_SHOWNA);
    }
    else
    {
        ShowWindow(MOverlayWindow, SW_HIDE);
    }
}

void CWindowOverlay::UpdatePosition()
{
    RECT Rect;
    GetWindowRect(MTargetWindow, &Rect);

    bool PositionChangedTopLeft = Rect.left != MLastRect.left || Rect.top != MLastRect.top;
    bool PositionChangedBottomRight = Rect.right != MLastRect.right || Rect.bottom != MLastRect.bottom;
    bool PositionChanged = PositionChangedTopLeft || PositionChangedBottomRight;

    if (PositionChanged)
    {
        int Width = Rect.right - Rect.left;
        int Height = Rect.bottom - Rect.top;
        if (Width < 1) Width = 1;
        if (Height < 1) Height = 1;

        SetWindowPos(MOverlayWindow, HWND_TOPMOST, Rect.left, Rect.top, Width, Height, SWP_NOACTIVATE);

        int OldWidth = MLastRect.right - MLastRect.left;
        int OldHeight = MLastRect.bottom - MLastRect.top;

        if (Width != OldWidth || Height != OldHeight)
        {
            if (MSwapChain)
            {
                MContext->GetContext()->OMSetRenderTargets(0, 0, 0);
                if (MRenderTargetView) { MRenderTargetView->Release(); MRenderTargetView = nullptr; }
                MSwapChain->ResizeBuffers(0, Width, Height, DXGI_FORMAT_UNKNOWN, 0);
                CreateSizeDependentResources(Width, Height);
            }
        }

        MLastRect = Rect;
    }
    else
    {
        SetWindowPos(MOverlayWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void CWindowOverlay::CreateSizeDependentResources(UINT Width, UINT Height)
{
    if (MRenderTargetView) { MRenderTargetView->Release(); MRenderTargetView = nullptr; }

    ID3D11Texture2D *BackBuffer = nullptr;
    MSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&BackBuffer);
    if (BackBuffer)
    {
        MContext->GetDevice()->CreateRenderTargetView(BackBuffer, nullptr, &MRenderTargetView);
        BackBuffer->Release();
    }
    D3D11_VIEWPORT Vp = { 0.0f, 0.0f, (float)Width, (float)Height, 0.0f, 1.0f };
    MContext->GetContext()->RSSetViewports(1, &Vp);
}