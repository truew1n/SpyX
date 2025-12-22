#include "WindowCaptureAPI.h"
#include "WindowCapture.h"
#include "Core/D3D11Context.h"

#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <queue>

// ============================================================================
// Thread-safe capture system with dedicated message loop thread
// ============================================================================

// Request types for the capture thread
enum class CaptureRequestType {
    Initialize,
    StartCapture,
    StopCapture,
    CaptureFrame,
    Cleanup,
    Shutdown
};

struct CaptureRequest {
    CaptureRequestType type;
    HWND hwnd = nullptr;  // For StartCapture
};

struct CaptureResponse {
    bool success = false;
    void* frameData = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::string error;
};

// Global state
static std::thread g_CaptureThread;
static std::atomic<bool> g_ThreadRunning{false};
static std::mutex g_RequestMutex;
static std::condition_variable g_RequestCV;
static std::condition_variable g_ResponseCV;
static CaptureRequest g_CurrentRequest;
static CaptureResponse g_CurrentResponse;
static std::atomic<bool> g_HasRequest{false};
static std::atomic<bool> g_HasResponse{false};

static std::string g_LastError;
static std::mutex g_ErrorMutex;
static char g_LastErrorBuffer[1024];  // Static buffer for returning error strings

// Capture thread state (only accessed from capture thread, except atomics)
static CD3D11Context* g_D3DContext = nullptr;
static CWindowCapture* g_WindowCapture = nullptr;
static std::atomic<bool> g_Initialized{false};
static std::atomic<bool> g_IsCapturing{false};  // True when actively capturing a window

// Last successful frame cache
static void* g_LastFrameData = nullptr;
static int g_LastFrameWidth = 0;
static int g_LastFrameHeight = 0;
static int g_LastFrameStride = 0;
static size_t g_LastFrameSize = 0;

// Helper to set error
static void SetError(const char* error) {
    std::lock_guard<std::mutex> lock(g_ErrorMutex);
    g_LastError = error ? error : "Unknown error";
}

// Helper to cache a successful frame
static void CacheFrame(void* data, int width, int height, int stride) {
    size_t dataSize = (size_t)stride * (size_t)height;
    
    // Reallocate if needed
    if (g_LastFrameData == nullptr || g_LastFrameSize < dataSize) {
        if (g_LastFrameData) {
            HeapFree(GetProcessHeap(), 0, g_LastFrameData);
        }
        g_LastFrameData = HeapAlloc(GetProcessHeap(), 0, dataSize);
        g_LastFrameSize = dataSize;
    }
    
    if (g_LastFrameData) {
        memcpy(g_LastFrameData, data, dataSize);
        g_LastFrameWidth = width;
        g_LastFrameHeight = height;
        g_LastFrameStride = stride;
    }
}

// Helper to return cached frame
static CaptureResponse GetCachedFrame() {
    CaptureResponse response;
    
    if (g_LastFrameData && g_LastFrameWidth > 0 && g_LastFrameHeight > 0) {
        size_t dataSize = (size_t)g_LastFrameStride * (size_t)g_LastFrameHeight;
        response.frameData = HeapAlloc(GetProcessHeap(), 0, dataSize);
        if (response.frameData) {
            memcpy(response.frameData, g_LastFrameData, dataSize);
            response.width = g_LastFrameWidth;
            response.height = g_LastFrameHeight;
            response.stride = g_LastFrameStride;
            response.success = true;
        }
    }
    
    return response;
}

// Process a single frame capture
static CaptureResponse ProcessCaptureFrame() {
    CaptureResponse response;
    
    if (!g_Initialized.load() || !g_WindowCapture || !g_WindowCapture->IsCapturing()) {
        response.error = "Not capturing";
        // Try to return cached frame
        CaptureResponse cached = GetCachedFrame();
        if (cached.success) {
            return cached;
        }
        return response;
    }
    
    // Wait for a new frame with 50ms timeout
    // This ensures we get a fresh frame after user input/rendering
    ID3D11Texture2D* texture = nullptr;
    HRESULT hr = g_WindowCapture->WaitForNewFrame(&texture, 50);
    if (FAILED(hr) || !texture) {
        response.error = "No frame available";
        // Try to return cached frame
        CaptureResponse cached = GetCachedFrame();
        if (cached.success) {
            return cached;
        }
        return response;
    }
    
    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    texture->GetDesc(&desc);
    
    // Validate dimensions
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > 8192 || desc.Height > 8192) {
        texture->Release();
        response.error = "Invalid texture dimensions";
        // Try to return cached frame
        CaptureResponse cached = GetCachedFrame();
        if (cached.success) {
            return cached;
        }
        return response;
    }
    
    response.width = static_cast<int>(desc.Width);
    response.height = static_cast<int>(desc.Height);
    
    // Create staging texture for CPU access
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    
    ID3D11Texture2D* stagingTexture = nullptr;
    hr = g_D3DContext->GetDevice()->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        texture->Release();
        response.error = "Failed to create staging texture";
        return response;
    }
    
    // Copy to staging texture
    g_D3DContext->GetContext()->CopyResource(stagingTexture, texture);
    texture->Release();
    
    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_D3DContext->GetContext()->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        stagingTexture->Release();
        response.error = "Failed to map staging texture";
        // Try to return cached frame
        CaptureResponse cached = GetCachedFrame();
        if (cached.success) {
            return cached;
        }
        return response;
    }
    
    response.stride = mapped.RowPitch;
    
    // Allocate output buffer and copy data
    size_t dataSize = (size_t)mapped.RowPitch * (size_t)desc.Height;
    
    // Use HeapAlloc for better Windows compatibility
    response.frameData = HeapAlloc(GetProcessHeap(), 0, dataSize);
    if (!response.frameData) {
        g_D3DContext->GetContext()->Unmap(stagingTexture, 0);
        stagingTexture->Release();
        response.error = "Failed to allocate memory";
        // Try to return cached frame
        CaptureResponse cached = GetCachedFrame();
        if (cached.success) {
            return cached;
        }
        return response;
    }
    
    memcpy(response.frameData, mapped.pData, dataSize);
    
    // Cache this successful frame for future fallback
    CacheFrame(response.frameData, response.width, response.height, response.stride);
    
    // Cleanup
    g_D3DContext->GetContext()->Unmap(stagingTexture, 0);
    stagingTexture->Release();
    
    response.success = true;
    return response;
}

// Capture thread main function
static void CaptureThreadMain() {
    // Initialize COM for this thread (required for WinRT)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        SetError("Failed to initialize COM in capture thread");
        g_ThreadRunning = false;
        return;
    }
    
    // Create message queue for this thread
    MSG msg;
    PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    
    while (g_ThreadRunning) {
        // Process Windows messages (required for WinRT callbacks)
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_ThreadRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        // Check for requests
        {
            std::unique_lock<std::mutex> lock(g_RequestMutex);
            if (g_RequestCV.wait_for(lock, std::chrono::milliseconds(1), [] { return g_HasRequest.load(); })) {
                CaptureRequest request = g_CurrentRequest;
                g_HasRequest = false;
                
                CaptureResponse response;
                
                switch (request.type) {
                    case CaptureRequestType::Initialize: {
                        if (g_Initialized) {
                            response.success = true;
                        } else {
                            // Create D3D11 context
                            g_D3DContext = new CD3D11Context();
                            hr = g_D3DContext->Initialize();
                            if (FAILED(hr)) {
                                delete g_D3DContext;
                                g_D3DContext = nullptr;
                                response.error = "Failed to initialize D3D11";
                            } else {
                                // Create window capture
                                g_WindowCapture = new CWindowCapture();
                                g_WindowCapture->Initialize(g_D3DContext);
                                g_Initialized = true;
                                response.success = true;
                            }
                        }
                        break;
                    }
                    
                    case CaptureRequestType::StartCapture: {
                        if (!g_Initialized || !g_WindowCapture) {
                            response.error = "Not initialized";
                        } else if (!IsWindow(request.hwnd)) {
                            response.error = "Invalid window handle";
                        } else {
                            hr = g_WindowCapture->StartCapture(request.hwnd);
                            if (FAILED(hr)) {
                                char errBuf[128];
                                sprintf_s(errBuf, "Failed to start capture (HRESULT: 0x%08X)", hr);
                                response.error = errBuf;
                            } else {
                                g_IsCapturing = true;
                                response.success = true;
                            }
                        }
                        break;
                    }
                    
                    case CaptureRequestType::StopCapture: {
                        g_IsCapturing = false;
                        if (g_WindowCapture) {
                            g_WindowCapture->StopCapture();
                        }
                        // Clear frame cache when stopping capture
                        if (g_LastFrameData) {
                            HeapFree(GetProcessHeap(), 0, g_LastFrameData);
                            g_LastFrameData = nullptr;
                            g_LastFrameWidth = 0;
                            g_LastFrameHeight = 0;
                            g_LastFrameStride = 0;
                            g_LastFrameSize = 0;
                        }
                        response.success = true;
                        break;
                    }
                    
                    case CaptureRequestType::CaptureFrame: {
                        response = ProcessCaptureFrame();
                        break;
                    }
                    
                    case CaptureRequestType::Cleanup: {
                        g_IsCapturing = false;
                        // Clear frame cache
                        if (g_LastFrameData) {
                            HeapFree(GetProcessHeap(), 0, g_LastFrameData);
                            g_LastFrameData = nullptr;
                            g_LastFrameWidth = 0;
                            g_LastFrameHeight = 0;
                            g_LastFrameStride = 0;
                            g_LastFrameSize = 0;
                        }
                        if (g_WindowCapture) {
                            g_WindowCapture->StopCapture();
                            delete g_WindowCapture;
                            g_WindowCapture = nullptr;
                        }
                        if (g_D3DContext) {
                            g_D3DContext->Cleanup();
                            delete g_D3DContext;
                            g_D3DContext = nullptr;
                        }
                        g_Initialized = false;
                        response.success = true;
                        break;
                    }
                    
                    case CaptureRequestType::Shutdown: {
                        g_IsCapturing = false;
                        if (g_WindowCapture) {
                            g_WindowCapture->StopCapture();
                            delete g_WindowCapture;
                            g_WindowCapture = nullptr;
                        }
                        if (g_D3DContext) {
                            g_D3DContext->Cleanup();
                            delete g_D3DContext;
                            g_D3DContext = nullptr;
                        }
                        g_Initialized = false;
                        g_ThreadRunning = false;
                        response.success = true;
                        break;
                    }
                }
                
                // Send response
                g_CurrentResponse = response;
                g_HasResponse = true;
                g_ResponseCV.notify_one();
            }
        }
    }
    
    CoUninitialize();
}

// Send request to capture thread and wait for response
static CaptureResponse SendRequest(const CaptureRequest& request, int timeoutMs = 5000) {
    CaptureResponse response;
    
    if (!g_ThreadRunning) {
        response.error = "Capture thread not running";
        return response;
    }
    
    {
        std::unique_lock<std::mutex> lock(g_RequestMutex);
        g_CurrentRequest = request;
        g_HasRequest = true;
        g_HasResponse = false;
        g_RequestCV.notify_one();
        
        // Wait for response
        if (!g_ResponseCV.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_HasResponse.load(); })) {
            response.error = "Request timed out";
            return response;
        }
        
        response = g_CurrentResponse;
    }
    
    return response;
}

// Start the capture thread
static bool StartCaptureThread() {
    if (g_ThreadRunning) {
        return true;
    }
    
    g_ThreadRunning = true;
    g_CaptureThread = std::thread(CaptureThreadMain);
    
    // Wait a bit for thread to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    return g_ThreadRunning;
}

// Stop the capture thread
static void StopCaptureThread() {
    if (!g_ThreadRunning) {
        return;
    }
    
    CaptureRequest request;
    request.type = CaptureRequestType::Shutdown;
    SendRequest(request, 2000);
    
    if (g_CaptureThread.joinable()) {
        g_CaptureThread.join();
    }
}

// ============================================================================
// Public API
// ============================================================================

extern "C" {

WC_API bool WC_Initialize() {
    if (!StartCaptureThread()) {
        SetError("Failed to start capture thread");
        return false;
    }
    
    CaptureRequest request;
    request.type = CaptureRequestType::Initialize;
    CaptureResponse response = SendRequest(request);
    
    if (!response.success) {
        SetError(response.error.c_str());
    }
    
    return response.success;
}

WC_API void WC_Cleanup() {
    CaptureRequest request;
    request.type = CaptureRequestType::Cleanup;
    SendRequest(request);
}

WC_API bool WC_StartCapture(HWND hwnd) {
    CaptureRequest request;
    request.type = CaptureRequestType::StartCapture;
    request.hwnd = hwnd;
    CaptureResponse response = SendRequest(request);
    
    if (!response.success) {
        SetError(response.error.c_str());
    }
    
    return response.success;
}

WC_API void WC_StopCapture() {
    CaptureRequest request;
    request.type = CaptureRequestType::StopCapture;
    SendRequest(request);
}

WC_API bool WC_IsCapturing() {
    // Only check atomic flags - thread-safe
    return g_ThreadRunning.load() && g_Initialized.load() && g_IsCapturing.load();
}

WC_API void* WC_CaptureFrame(int* outWidth, int* outHeight, int* outStride) {
    if (!outWidth || !outHeight || !outStride) {
        SetError("Invalid parameters");
        return nullptr;
    }
    
    if (!g_ThreadRunning) {
        SetError("Capture thread not running");
        return nullptr;
    }
    
    CaptureRequest request;
    request.type = CaptureRequestType::CaptureFrame;
    CaptureResponse response = SendRequest(request, 1000);  // 1 second timeout for frame capture
    
    if (!response.success) {
        SetError(response.error.c_str());
        return nullptr;
    }
    
    // Validate response data
    if (response.frameData == nullptr) {
        SetError("Frame data is null despite success");
        return nullptr;
    }
    
    if (response.width <= 0 || response.height <= 0 || response.stride <= 0) {
        SetError("Invalid frame dimensions in response");
        if (response.frameData) {
            free(response.frameData);
        }
        return nullptr;
    }
    
    *outWidth = response.width;
    *outHeight = response.height;
    *outStride = response.stride;
    
    return response.frameData;
}

WC_API bool WC_CaptureFrameInfo(WC_FrameInfo* outInfo) {
    if (!outInfo) {
        SetError("Invalid parameter: outInfo is null");
        return false;
    }
    
    // Zero out the struct
    outInfo->width = 0;
    outInfo->height = 0;
    outInfo->stride = 0;
    outInfo->data = nullptr;
    
    if (!g_ThreadRunning) {
        SetError("Capture thread not running");
        return false;
    }
    
    CaptureRequest request;
    request.type = CaptureRequestType::CaptureFrame;
    CaptureResponse response = SendRequest(request, 1000);
    
    if (!response.success) {
        SetError(response.error.c_str());
        return false;
    }
    
    if (response.frameData == nullptr) {
        SetError("Frame data is null despite success");
        return false;
    }
    
    if (response.width <= 0 || response.height <= 0 || response.stride <= 0) {
        SetError("Invalid frame dimensions in response");
        if (response.frameData) {
            free(response.frameData);
        }
        return false;
    }
    
    outInfo->width = response.width;
    outInfo->height = response.height;
    outInfo->stride = response.stride;
    outInfo->data = response.frameData;
    
    return true;
}

// Global variable to cache last frame buffer size for WC_GetFrameBufferSize
static std::atomic<int> g_CachedBufferSize{0};

WC_API int WC_GetFrameBufferSize() {
    if (!g_ThreadRunning.load() || !g_IsCapturing.load()) {
        return 0;
    }
    
    // Return cached size if available
    int cached = g_CachedBufferSize.load();
    if (cached > 0) {
        return cached;
    }
    
    // Capture a frame to determine size
    CaptureRequest request;
    request.type = CaptureRequestType::CaptureFrame;
    CaptureResponse response = SendRequest(request, 1000);
    
    if (!response.success || response.frameData == nullptr) {
        return 0;
    }
    
    int size = response.stride * response.height;
    g_CachedBufferSize = size;
    
    // Free the frame data since we're just checking size
    HeapFree(GetProcessHeap(), 0, response.frameData);
    
    return size;
}

WC_API int WC_CaptureFrameToBuffer(void* buffer, int bufferSize, int* outWidth, int* outHeight, int* outStride) {
    if (!buffer || bufferSize <= 0 || !outWidth || !outHeight || !outStride) {
        SetError("Invalid parameters");
        return 0;
    }
    
    *outWidth = 0;
    *outHeight = 0;
    *outStride = 0;
    
    if (!g_ThreadRunning.load()) {
        SetError("Capture thread not running");
        return 0;
    }
    
    CaptureRequest request;
    request.type = CaptureRequestType::CaptureFrame;
    CaptureResponse response = SendRequest(request, 1000);
    
    if (!response.success) {
        SetError(response.error.c_str());
        return 0;
    }
    
    if (response.frameData == nullptr) {
        SetError("Frame data is null despite success");
        return 0;
    }
    
    int requiredSize = response.stride * response.height;
    
    if (bufferSize < requiredSize) {
        SetError("Buffer too small");
        HeapFree(GetProcessHeap(), 0, response.frameData);
        // Return negative of required size to indicate how much is needed
        return -requiredSize;
    }
    
    // Copy data to provided buffer
    memcpy(buffer, response.frameData, requiredSize);
    
    // Free the DLL-allocated memory
    HeapFree(GetProcessHeap(), 0, response.frameData);
    
    *outWidth = response.width;
    *outHeight = response.height;
    *outStride = response.stride;
    
    // Cache the size for future GetFrameBufferSize calls
    g_CachedBufferSize = requiredSize;
    
    return requiredSize;
}

WC_API void WC_FreeFrame(void* frameData) {
    if (frameData != nullptr) {
        HeapFree(GetProcessHeap(), 0, frameData);
    }
}

WC_API const char* WC_GetLastError() {
    std::lock_guard<std::mutex> lock(g_ErrorMutex);
    strncpy_s(g_LastErrorBuffer, sizeof(g_LastErrorBuffer), g_LastError.c_str(), _TRUNCATE);
    return g_LastErrorBuffer;
}

WC_API void WC_Shutdown() {
    StopCaptureThread();
}

} // extern "C"
