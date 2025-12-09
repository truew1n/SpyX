#ifndef TAPI_WINDOW_OVERLAY_H
#define TAPI_WINDOW_OVERLAY_H

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>

#include "Core/D3D11Context.h" 
#include "Core/Delegate.h"


class CD3D11Context;

using FRenderDelegate = TDelegate<void(ID3D11DeviceContext *DeviceContext, ID3D11RenderTargetView *RenderTargetView)>;
using FWindowProcedureDelegate = TDelegate<HRESULT(HWND WindowHandle, UINT Message, WPARAM WParameter, LPARAM LParameter, LRESULT *OutResult)>;

class CWindowOverlay
{
public:
    CWindowOverlay();
    ~CWindowOverlay();

    bool Initialize(CD3D11Context *Context);

    bool BindToWindow(HWND TargetWindow);

    void SetRenderCallback(FRenderDelegate Callback);
    void SetWindowProcedureCallback(FWindowProcedureDelegate Callback);

    void SetOverlayVisibility(bool NewOverlayVisibility);
    bool GetOverlayVisibility();

    void Update();

    void Render();

    HWND GetHandle() const { return MOverlayWindow; }
    HWND GetTargetWindow() const { return MTargetWindow; }
    bool IsValid() const;

    void SetContentProtection(bool Enabled);

private:
    static LRESULT CALLBACK WindowProcedure(HWND WindowHandle, UINT Message, WPARAM WParameter, LPARAM LParameter);
    void UpdateVisiblity();
    void UpdatePosition();
    void CreateSizeDependentResources(UINT Width, UINT Height);

    CD3D11Context *MContext = nullptr;

    HWND MOverlayWindow = nullptr;
    HWND MTargetWindow = nullptr;

    IDXGISwapChain1 *MSwapChain = nullptr;
    ID3D11RenderTargetView *MRenderTargetView = nullptr;
    ID3D11BlendState *MBlendState = nullptr;

    bool MOverlayVisibility = true;

    FRenderDelegate MRenderCallback;
    FWindowProcedureDelegate MWindowProcedureCallback;
    RECT MLastRect = { 0 };
};

#endif