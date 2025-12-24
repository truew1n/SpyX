#include "WindowCapture.h"

// WinRT Headers
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <chrono> 

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "dwmapi.lib")

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WDX = winrt::Windows::Graphics::DirectX;
namespace WDT = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace WF = winrt::Windows::Foundation;
namespace WG = winrt::Windows::Graphics;


struct CWindowCapture::SImplementation
{
    CWindowCapture *MParent = nullptr;

    WGC::GraphicsCaptureItem MItem{ nullptr };
    WGC::Direct3D11CaptureFramePool MFramePool{ nullptr };
    WGC::GraphicsCaptureSession MSession{ nullptr };
    WDT::IDirect3DDevice MDevice{ nullptr };
    WG::SizeInt32 MLastSize{ 0, 0 };

    HRESULT CreateCaptureItem(HWND HWnd);
    void OnFrameArrived(WGC::Direct3D11CaptureFramePool const &Sender, WF::IInspectable const &Args);
};


void CreateDispatcherQueue()
{
    DispatcherQueueOptions Options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
    ABI::Windows::System::IDispatcherQueueController *Controller = nullptr;
    CreateDispatcherQueueController(Options, &Controller);
    if (Controller) Controller->Release();
}


CWindowCapture::CWindowCapture()
{
    MImplementation = new SImplementation();
    MImplementation->MParent = this;
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    CreateDispatcherQueue();
}

CWindowCapture::~CWindowCapture()
{
    StopCapture();
    delete MImplementation;
}


void CWindowCapture::Initialize(CD3D11Context *Context) { MContext = Context; }
void CWindowCapture::SetCallback(FFrameDelegate Callback) { MFrameCallback = Callback; }

bool CWindowCapture::IsCapturing() const
{
    return MIsCapturing;
}

HRESULT CWindowCapture::AcquireLatestFrame(ID3D11Texture2D **OutTexture)
{
    if (!OutTexture) return E_INVALIDARG;
    *OutTexture = nullptr;

    std::lock_guard<std::mutex> Lock(MMutex);

    if (MLatestFrame)
    {
        MLatestFrame->AddRef();
        *OutTexture = MLatestFrame;
        return S_OK;
    }
    return S_FALSE;
}

void CWindowCapture::OnFrameReceived(ID3D11Texture2D *Texture)
{
    {
        std::lock_guard<std::mutex> Lock(MMutex);
        if (MLatestFrame) MLatestFrame->Release();
        MLatestFrame = Texture;
        MLatestFrame->AddRef();
        MFrameCount++;  // Increment frame counter
    }

    if (MFrameCallback.IsBound())
    {
        MFrameCallback.Execute(Texture);
    }
}

HRESULT CWindowCapture::WaitForNewFrame(ID3D11Texture2D **OutTexture, int timeoutMs)
{
    if (!OutTexture) return E_INVALIDARG;
    *OutTexture = nullptr;
    
    if (!MIsCapturing) return E_FAIL;
    
    uint64_t startFrame = MFrameCount.load();
    auto startTime = std::chrono::steady_clock::now();
    
    while (true) {
        // Check if new frame arrived
        if (MFrameCount.load() > startFrame) {
            return AcquireLatestFrame(OutTexture);
        }
        
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            // Timeout - return whatever we have (even if old)
            return AcquireLatestFrame(OutTexture);
        }
        
        // Small sleep to not burn CPU
        Sleep(1);
    }
}

HRESULT CWindowCapture::StartCapture(HWND WindowHandle)
{
    if (!MContext || !MContext->GetDevice()) return E_UNEXPECTED;

    StopCapture();

    HRESULT HResult = MImplementation->CreateCaptureItem(WindowHandle);
    if (FAILED(HResult)) return HResult; // Returns specific HRESULT from CreateCaptureItem

    WDT::IDirect3DDevice Direct3DDevice{ nullptr };
    HResult = MContext->CreateDirect3DDevice(winrt::put_abi(Direct3DDevice));
    if (FAILED(HResult)) return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0002); // D3D device creation failed

    MImplementation->MDevice = Direct3DDevice;
    MImplementation->MLastSize = MImplementation->MItem.Size();

    try
    {
        MImplementation->MFramePool = WGC::Direct3D11CaptureFramePool::Create(
            MImplementation->MDevice,
            WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            MImplementation->MLastSize
        );

        MImplementation->MSession = MImplementation->MFramePool.CreateCaptureSession(MImplementation->MItem);

        if (WF::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
        {
            MImplementation->MSession.IsBorderRequired(false);
        }

        MImplementation->MFramePool.FrameArrived({ MImplementation, &SImplementation::OnFrameArrived });

        MImplementation->MSession.StartCapture();

        MIsCapturing = true;
    }
    catch (...) { return E_FAIL; }

    return S_OK;
}

void CWindowCapture::StopCapture()
{
    MIsCapturing = false;
    try {
        if (MImplementation->MSession) { MImplementation->MSession.Close(); MImplementation->MSession = nullptr; }
        if (MImplementation->MFramePool) { MImplementation->MFramePool.Close(); MImplementation->MFramePool = nullptr; }
    }
    catch (...) {}

    MImplementation->MItem = nullptr;
    MImplementation->MDevice = nullptr;

    std::lock_guard<std::mutex> Lock(MMutex);
    if (MLatestFrame) { MLatestFrame->Release(); MLatestFrame = nullptr; }
}


HRESULT CWindowCapture::SImplementation::CreateCaptureItem(HWND HWnd)
{
    // Validate window handle first
    if (!HWnd || !IsWindow(HWnd)) {
        OutputDebugStringA("[WindowCapture] Invalid window handle\n");
        return E_HANDLE;
    }
    
    // Log window info for diagnostics (don't fail on these - WGC might still work)
    LONG style = GetWindowLongA(HWnd, GWL_STYLE);
    LONG exStyle = GetWindowLongA(HWnd, GWL_EXSTYLE);
    char buf[512];
    sprintf_s(buf, "[WindowCapture] Window styles: 0x%08X, exStyles: 0x%08X, WS_VISIBLE=%d\n",
        style, exStyle, (style & WS_VISIBLE) ? 1 : 0);
    OutputDebugStringA(buf);
    
    // Get window rect
    RECT rect;
    if (GetWindowRect(HWnd, &rect)) {
        sprintf_s(buf, "[WindowCapture] Window rect: %d,%d - %d,%d (size: %dx%d)\n",
            rect.left, rect.top, rect.right, rect.bottom,
            rect.right - rect.left, rect.bottom - rect.top);
        OutputDebugStringA(buf);
    }
    
    // Check if window is iconic (minimized) - this IS a hard fail
    if (IsIconic(HWnd)) {
        OutputDebugStringA("[WindowCapture] Window is minimized - cannot capture\n");
        return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0011); // Custom: Window minimized
    }
    
    // Check DWM cloaking (window might be on another virtual desktop)
    BOOL isCloaked = FALSE;
    DwmGetWindowAttribute(HWnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (isCloaked) {
        OutputDebugStringA("[WindowCapture] Window is DWM-cloaked (possibly on another virtual desktop)\n");
        // Don't fail - WGC might still work
    }
    
    // Log IsWindowVisible result but don't fail on it
    // MTA and some games have complex window hierarchies where this returns false
    if (!IsWindowVisible(HWnd)) {
        OutputDebugStringA("[WindowCapture] WARNING: IsWindowVisible returned false, but attempting capture anyway\n");
    }
    
    auto ActivationFactory = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
    auto InteropFactory = ActivationFactory.as<IGraphicsCaptureItemInterop>();
    if (!InteropFactory) return E_NOINTERFACE;

    HRESULT hr = InteropFactory->CreateForWindow(
        HWnd,
        winrt::guid_of<WGC::GraphicsCaptureItem>(),
        winrt::put_abi(MItem)
    );
    
    if (FAILED(hr)) {
        sprintf_s(buf, "[WindowCapture] CreateForWindow failed: 0x%08X\n", hr);
        OutputDebugStringA(buf);
    }
    
    return hr;
}

void CWindowCapture::SImplementation::OnFrameArrived(WGC::Direct3D11CaptureFramePool const &Sender, WF::IInspectable const &)
{
    auto Frame = Sender.TryGetNextFrame();
    if (!Frame) return;

    auto ContentSize = Frame.ContentSize();
    if (ContentSize.Width != MLastSize.Width || ContentSize.Height != MLastSize.Height)
    {
        MLastSize = ContentSize;
        MFramePool.Recreate(
            MDevice,
            WDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            MLastSize
        );
    }

    auto Surface = Frame.Surface();
    auto Access = Surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    if (!Access) return;

    ID3D11Texture2D *Texture = nullptr;
    if (SUCCEEDED(Access->GetInterface(__uuidof(ID3D11Texture2D), (void **)&Texture)))
    {
        MParent->OnFrameReceived(Texture);
        Texture->Release();
    }
}