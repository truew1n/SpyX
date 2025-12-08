#include "WindowCapture.h"

// WinRT Headers
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <DispatcherQueue.h> 

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
    }

    if (MFrameCallback.IsBound())
    {
        MFrameCallback.Execute(Texture);
    }
}

HRESULT CWindowCapture::StartCapture(HWND WindowHandle)
{
    if (!MContext || !MContext->GetDevice()) return E_UNEXPECTED;

    StopCapture();

    HRESULT HResult = MImplementation->CreateCaptureItem(WindowHandle);
    if (FAILED(HResult)) return HResult;

    WDT::IDirect3DDevice Direct3DDevice{ nullptr };
    HResult = MContext->CreateDirect3DDevice(winrt::put_abi(Direct3DDevice));
    if (FAILED(HResult)) return HResult;

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
    auto ActivationFactory = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
    auto InteropFactory = ActivationFactory.as<IGraphicsCaptureItemInterop>();
    if (!InteropFactory) return E_NOINTERFACE;

    return InteropFactory->CreateForWindow(
        HWnd,
        winrt::guid_of<WGC::GraphicsCaptureItem>(),
        winrt::put_abi(MItem)
    );
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