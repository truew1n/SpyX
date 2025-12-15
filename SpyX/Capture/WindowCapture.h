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
    
    // Wait for a new frame with timeout (milliseconds). Returns frame count or 0 on timeout.
    HRESULT WaitForNewFrame(ID3D11Texture2D **OutTexture, int timeoutMs);
    
    // Get current frame counter
    uint64_t GetFrameCount() const { return MFrameCount.load(); }
    
    bool IsCapturing() const;

private:
    void OnFrameReceived(ID3D11Texture2D *Texture);

    struct SImplementation;
    SImplementation *MImplementation = nullptr;

    std::mutex MMutex;
    ID3D11Texture2D *MLatestFrame = nullptr;
    std::atomic<bool> MIsCapturing = false;
    std::atomic<uint64_t> MFrameCount = 0;  // Frame counter for detecting new frames

    CD3D11Context *MContext = nullptr;
    FFrameDelegate MFrameCallback;

    friend struct SImplementation;
};

#endif