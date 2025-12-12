#include "SpyXBridge.h"
#include "../Overlay/WindowOverlay.h"
#include "../Core/D3D11Context.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <string>
#include <vector>

// Globals
static CD3D11Context* g_Context = nullptr;
static CWindowOverlay* g_Overlay = nullptr;
static bool g_Initialized = false;
static bool g_Bound = false;

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Custom Window Procedure to hook into Overlay's window proc
static HRESULT WindowProcCallback(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* outResult) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        *outResult = true;
        return S_OK;
    }
    return S_FALSE;
}

// Render Callback
static void RenderCallback(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

extern "C" {

SPYX_API bool SpyX_Initialize() {
    if (g_Initialized) return true;

    g_Context = new CD3D11Context();
    if (FAILED(g_Context->Initialize())) {
        delete g_Context;
        g_Context = nullptr;
        return false;
    }

    g_Overlay = new CWindowOverlay();
    if (!g_Overlay->Initialize(g_Context)) {
        delete g_Overlay;
        delete g_Context;
        g_Overlay = nullptr;
        g_Context = nullptr;
        return false;
    }

    // Setup ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    
    // Init DX11 backend (Device is available now)
    ImGui_ImplDX11_Init(g_Context->GetDevice(), g_Context->GetContext());

    // Setup Callbacks
    FRenderDelegate renderDelegate;
    renderDelegate.BindStatic(RenderCallback);
    g_Overlay->SetRenderCallback(renderDelegate);

    FWindowProcedureDelegate wndProcDelegate;
    wndProcDelegate.BindStatic(WindowProcCallback);
    g_Overlay->SetWindowProcedureCallback(wndProcDelegate);

    g_Initialized = true;
    return true;
}

SPYX_API bool SpyX_BindToWindow(const char* windowName) {
    if (!g_Initialized) return false;
    if (g_Bound) return true; // Already bound
    
    // Find window by name
    std::string name(windowName);
    std::wstring wname(name.begin(), name.end());
    HWND target = FindWindowW(nullptr, wname.c_str());
    
    if (!target) return false;

    if (!g_Overlay->BindToWindow(target)) {
        return false;
    }

    // Init Win32 backend (Window Handle is available now)
    ImGui_ImplWin32_Init(g_Overlay->GetHandle());
    
    g_Bound = true;
    return true;
}

SPYX_API void SpyX_Cleanup() {
    if (!g_Initialized) return;

    if (g_Bound) {
        ImGui_ImplWin32_Shutdown();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();

    delete g_Overlay;
    g_Overlay = nullptr;

    g_Context->Cleanup();
    delete g_Context;
    g_Context = nullptr;

    g_Initialized = false;
    g_Bound = false;
}

SPYX_API void SpyX_BeginFrame() {
    if (!g_Initialized || !g_Bound) return;

    // Pump messages
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Update Overlay Logic (Position, etc)
    g_Overlay->Update();

    // Start ImGui Frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Create a full-screen background window for drawing
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##BackBuffer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
    
    // We end this immediately so the user can start their own windows if they want.
    // But wait, if we end it, the DrawList is closed.
    // The previous design assumed the user only draws primitives on the background.
    // Now the user wants "Full Access", so they might want to create their own Windows.
    // So we should NOT force a background window if they want to use SpyX_Begin.
    // However, for backward compatibility with SpyX_DrawBox, we need a current window.
    // Let's keep the background window open, but the user can create new windows ON TOP of it.
    // ImGui allows nested Begin/End or parallel Begin/End.
}

SPYX_API void SpyX_EndFrame() {
    if (!g_Initialized || !g_Bound) return;

    ImGui::End(); // End ##BackBuffer

    // Render calls the callback which calls ImGui::Render
    g_Overlay->Render();
}

SPYX_API bool SpyX_Begin(const char* name, bool* p_open, int flags) {
    if (!g_Initialized || !g_Bound) return false;
    // We need to step out of the ##BackBuffer to create a new window
    ImGui::End(); 
    
    bool res = ImGui::Begin(name, p_open, flags);
    
    // If we return, the user will eventually call SpyX_End().
    // But SpyX_EndFrame expects to close ##BackBuffer.
    // This is tricky. The structure must be:
    // BeginFrame -> Begin(BackBuffer) -> ... -> End(BackBuffer) -> EndFrame
    // If user calls SpyX_Begin, they are effectively doing:
    // BeginFrame -> Begin(BackBuffer) -> End(BackBuffer) -> Begin(User) -> ... -> End(User) -> Begin(BackBuffer) -> EndFrame?
    
    // Let's change the strategy.
    // SpyX_BeginFrame starts the frame but DOES NOT begin the BackBuffer window automatically?
    // No, SpyX_DrawBox needs it.
    
    // Correct strategy:
    // SpyX_BeginFrame -> ImGui::NewFrame(); Begin("##BackBuffer");
    // SpyX_Begin -> End("##BackBuffer"); Begin("UserWindow");
    // SpyX_End -> End("UserWindow"); Begin("##BackBuffer");
    // SpyX_EndFrame -> End("##BackBuffer"); Render();
    
    return res;
}

SPYX_API void SpyX_End() {
    if (!g_Initialized || !g_Bound) return;
    ImGui::End();
    
    // Re-enter BackBuffer for subsequent draw calls or EndFrame
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##BackBuffer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
}

SPYX_API bool SpyX_Button(const char* label, float w, float h) {
    if (!g_Initialized || !g_Bound) return false;
    return ImGui::Button(label, ImVec2(w, h));
}

SPYX_API bool SpyX_Checkbox(const char* label, bool* v) {
    if (!g_Initialized || !g_Bound) return false;
    return ImGui::Checkbox(label, v);
}

SPYX_API bool SpyX_InputText(const char* label, char* buf, size_t buf_size) {
    if (!g_Initialized || !g_Bound) return false;
    return ImGui::InputText(label, buf, buf_size);
}

SPYX_API bool SpyX_SliderFloat(const char* label, float* v, float v_min, float v_max) {
    if (!g_Initialized || !g_Bound) return false;
    return ImGui::SliderFloat(label, v, v_min, v_max);
}

SPYX_API bool SpyX_ColorEdit4(const char* label, float* col) {
    if (!g_Initialized || !g_Bound) return false;
    return ImGui::ColorEdit4(label, col);
}

SPYX_API void SpyX_SameLine(float offset_from_start_x, float spacing) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::SameLine(offset_from_start_x, spacing);
}

SPYX_API void SpyX_Separator() {
    if (!g_Initialized || !g_Bound) return;
    ImGui::Separator();
}

SPYX_API void SpyX_Text(const char* text) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::Text("%s", text);
}

SPYX_API void SpyX_SetNextWindowPos(float x, float y, int cond) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
}

SPYX_API void SpyX_SetNextWindowSize(float w, float h, int cond) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);
}

SPYX_API void SpyX_SetNextWindowBgAlpha(float alpha) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::SetNextWindowBgAlpha(alpha);
}


SPYX_API void SpyX_DrawBox(float x, float y, float w, float h, float r, float g, float b, float a, float thickness) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)), 0.0f, 0, thickness);
}

SPYX_API void SpyX_DrawFilledBox(float x, float y, float w, float h, float r, float g, float b, float a) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)));
}

SPYX_API void SpyX_DrawText(float x, float y, const char* text, float r, float g, float b, float a, float fontSize) {
    if (!g_Initialized || !g_Bound) return;
    // Note: fontSize handling in ImGui is a bit complex (requires loading fonts of different sizes).
    // For now we ignore fontSize or use Scale.
    // ImGui::SetWindowFontScale(fontSize / 13.0f); // Example
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)), text);
    // ImGui::SetWindowFontScale(1.0f);
}

SPYX_API void SpyX_DrawLine(float x1, float y1, float x2, float y2, float r, float g, float b, float a, float thickness) {
    if (!g_Initialized || !g_Bound) return;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)), thickness);
}

}
