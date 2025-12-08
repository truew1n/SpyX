#ifndef TAPI_WINDOW_CAPTURE_H
#define TAPI_WINDOW_CAPTURE_H

#include "Core/D3D11Context.h" 
#include "Core/Delegate.h"

#include <mutex>
#include <atomic>
#include <d3d11.h>

using FFrameDelegate = TDelegate<void(ID3D11Texture2D *)>;

class CWindowCapture
{
public:
    CWindowCapture();
    ~CWindowCapture();

    void Initialize(CD3D11Context *Context);
    void SetCallback(FFrameDelegate Callback);

    HRESULT StartCapture(HWND WindowHandle);
    void StopCapture();

    HRESULT AcquireLatestFrame(ID3D11Texture2D **OutTexture);
    bool IsCapturing() const;

private:
    void OnFrameReceived(ID3D11Texture2D *Texture);

    struct SImplementation;
    SImplementation *MImplementation = nullptr;

    std::mutex MMutex;
    ID3D11Texture2D *MLatestFrame = nullptr;
    std::atomic<bool> MIsCapturing = false;

    CD3D11Context *MContext = nullptr;
    FFrameDelegate MFrameCallback;

    friend struct SImplementation;
};

#endif