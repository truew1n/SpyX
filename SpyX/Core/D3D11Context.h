#ifndef TAPI_D3D11_CONTEXT_H
#define TAPI_D3D11_CONTEXT_H

#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class CD3D11Context
{
public:
    CD3D11Context() = default;
    ~CD3D11Context();

    HRESULT Initialize();

    void Cleanup();

    ID3D11Device *GetDevice() const;
    ID3D11DeviceContext *GetContext() const;

    HRESULT CreateDirect3DDevice(void **OutDevice);

private:
    ID3D11Device *MD3DDevice = nullptr;
    ID3D11DeviceContext *MD3DContext = nullptr;
};

#endif