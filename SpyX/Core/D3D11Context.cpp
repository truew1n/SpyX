#include "D3D11Context.h"
#include <dxgi1_2.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

CD3D11Context::~CD3D11Context()
{
    Cleanup();
}

HRESULT CD3D11Context::Initialize()
{
    D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT CreationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    return D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        CreationFlags,
        FeatureLevels,
        1,
        D3D11_SDK_VERSION,
        &MD3DDevice,
        nullptr,
        &MD3DContext
    );
}

void CD3D11Context::Cleanup()
{
    if (MD3DContext)
    {
        MD3DContext->Release();
        MD3DContext = nullptr;
    }

    if (MD3DDevice)
    {
        MD3DDevice->Release();
        MD3DDevice = nullptr;
    }
}

ID3D11Device *CD3D11Context::GetDevice() const
{
    return MD3DDevice;
}

ID3D11DeviceContext *CD3D11Context::GetContext() const
{
    return MD3DContext;
}

HRESULT CD3D11Context::CreateDirect3DDevice(void **OutDevice)
{
    if (!MD3DDevice)
    {
        return E_POINTER;
    }

    IDXGIDevice *DXGIDevice = nullptr;
    HRESULT HResult = MD3DDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&DXGIDevice);
    if (FAILED(HResult))
    {
        return HResult;
    }

    winrt::com_ptr<::IInspectable> Inspectable;
    HResult = CreateDirect3D11DeviceFromDXGIDevice(DXGIDevice, Inspectable.put());
    DXGIDevice->Release();

    if (SUCCEEDED(HResult))
    {
        *OutDevice = winrt::detach_abi(Inspectable.try_as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>());
        if (!OutDevice) HResult = E_NOINTERFACE;
    }
    return HResult;
}