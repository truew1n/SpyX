#ifndef WINDOW_CAPTURE_API_H
#define WINDOW_CAPTURE_API_H

#include <windows.h>

#ifdef WINDOWCAPTURE_EXPORTS
#define WC_API __declspec(dllexport)
#else
#define WC_API __declspec(dllimport)
#endif

// Frame info structure for passing frame metadata
// Using default alignment for JNA compatibility
typedef struct WC_FrameInfo {
    int width;
    int height;
    int stride;
    void* data;  // Pointer to pixel data (BGRA format)
} WC_FrameInfo;

extern "C" {

/**
 * Initialize the capture system.
 * Must be called before any other functions.
 * @return true if successful
 */
WC_API bool WC_Initialize();

/**
 * Cleanup and release all resources.
 */
WC_API void WC_Cleanup();

/**
 * Start capturing a window.
 * @param hwnd Handle to the window to capture
 * @return true if capture started successfully
 */
WC_API bool WC_StartCapture(HWND hwnd);

/**
 * Stop the current capture session.
 */
WC_API void WC_StopCapture();

/**
 * Check if currently capturing.
 * @return true if a capture session is active
 */
WC_API bool WC_IsCapturing();

/**
 * Capture the latest frame as raw BGRA pixel data.
 * @param outWidth Pointer to receive frame width
 * @param outHeight Pointer to receive frame height  
 * @param outStride Pointer to receive row stride in bytes
 * @return Pointer to pixel data (must be freed with WC_FreeFrame), or nullptr if no frame available
 */
WC_API void* WC_CaptureFrame(int* outWidth, int* outHeight, int* outStride);

/**
 * Capture frame directly into a Java-allocated buffer.
 * @param buffer Pre-allocated buffer to receive pixel data
 * @param bufferSize Size of the buffer in bytes
 * @param outWidth Pointer to receive frame width
 * @param outHeight Pointer to receive frame height
 * @param outStride Pointer to receive row stride in bytes
 * @return Number of bytes written, or 0 on failure
 */
WC_API int WC_CaptureFrameToBuffer(void* buffer, int bufferSize, int* outWidth, int* outHeight, int* outStride);

/**
 * Get the expected buffer size for capturing a frame.
 * @return Required buffer size in bytes, or 0 if not capturing
 */
WC_API int WC_GetFrameBufferSize();

/**
 * Capture the latest frame and fill in a frame info structure.
 * @param outInfo Pointer to WC_FrameInfo structure to fill
 * @return true if successful, false if no frame available
 */
WC_API bool WC_CaptureFrameInfo(WC_FrameInfo* outInfo);

/**
 * Free a frame previously returned by WC_CaptureFrame or WC_CaptureFrameInfo.
 * @param frameData The pointer returned by WC_CaptureFrame or in WC_FrameInfo.data
 */
WC_API void WC_FreeFrame(void* frameData);

/**
 * Get the last error message.
 * @return Error message string (do not free)
 */
WC_API const char* WC_GetLastError();

/**
 * Shutdown the capture system and stop the capture thread.
 * Call this when completely done with capture.
 */
WC_API void WC_Shutdown();

}

#endif // WINDOW_CAPTURE_API_H
