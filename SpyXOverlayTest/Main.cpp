#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <string>

// --- IMGUI INCLUDES ---
// Make sure you have the ImGui files added to your project!
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Adjust these paths if your folder structure is different
#include "Core/D3D11Context.h"
#include "Overlay/WindowOverlay.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

// =============================================================
// GLOBALS
// =============================================================
CD3D11Context     g_D3DContext;
CWindowOverlay    g_Overlay;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool IsCursorOverImGui()
{
    if (ImGui::GetCurrentContext() == nullptr) return false;

    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse) return true;

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup)) return true;

    return false;
}

bool g_state = 1;

HRESULT OnOverlayWindowMessage(HWND WindowHandle, UINT Message, WPARAM WParameter, LPARAM LParameter, LRESULT *OutResult)
{
    // 1. Give inputs to ImGui first
    if (ImGui_ImplWin32_WndProcHandler(WindowHandle, Message, WParameter, LParameter))
    {
        *OutResult = true;
        return S_OK;
    }

    g_state = IsCursorOverImGui();

    switch (Message)
    {
    case WM_MOUSEACTIVATE:
        *OutResult = MA_NOACTIVATE;
        return S_OK;

    case WM_NCHITTEST:
        if (IsCursorOverImGui())
        {
            *OutResult = HTCLIENT;
            return S_OK;
        }
        *OutResult = HTTRANSPARENT;
        return S_OK;

    case WM_SETCURSOR:
        if (IsCursorOverImGui())
        {
            return S_FALSE;
        }
        break;
    }

    return S_FALSE;
}

// =============================================================
// RENDER CALLBACK (IMGUI)
// =============================================================
void ImGuiRenderLoop(ID3D11DeviceContext *DeviceContext, ID3D11RenderTargetView *RenderTargetView)
{
    ImGuiIO &io = ImGui::GetIO();
    POINT p;
    if (GetCursorPos(&p))
    {
        ScreenToClient(g_Overlay.GetHandle(), &p);
        io.MousePos = ImVec2((float)p.x, (float)p.y);
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }
    
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        ImGui::Begin("SpyX Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Target Window Detected!");
        ImGui::Separator();

        // --- TEST 1: PRZYCISK (Sprawdza klikniêcie myszk¹) ---
        static int clickCount = 0;
        if (ImGui::Button("Click Me Test"))
        {
            clickCount++;
        }
        ImGui::SameLine();
        ImGui::Text("Clicks: %d", clickCount);
        ImGui::Text("State: %d", g_state);

        // --- TEST 2: POLE TEKSTOWE (Sprawdza input klawiatury) ---
        // Uwaga: Jeœli u¿ywamy MA_NOACTIVATE, pisanie mo¿e wymagaæ dodatkowej logiki,
        // ale to pole pozwoli sprawdziæ, czy ImGui w ogóle dostaje focus.
        static char textBuffer[128] = "Type here...";
        ImGui::InputText("Text Input", textBuffer, sizeof(textBuffer));

        ImGui::Separator();

        static float color[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        ImGui::ColorEdit4("ESP Color", color);

        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

        ImGui::End();
    }

    ImGui::Render();

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// =============================================================
// WINDOW PICKER (Unchanged)
// =============================================================
struct WindowInfo { HWND hwnd; std::wstring title; };
std::vector<WindowInfo> g_WindowList;
HWND g_hSelectorList = NULL;
HWND g_hSelectedWindow = NULL;

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT r; GetWindowRect(hwnd, &r);
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return TRUE;
    wchar_t className[256]; GetClassName(hwnd, className, 256);
    if (wcscmp(className, L"Progman") == 0) return TRUE;
    wchar_t title[256]; GetWindowTextW(hwnd, title, 256);
    if (wcslen(title) > 0) g_WindowList.push_back({ hwnd, title });
    return TRUE;
}

LRESULT CALLBACK SelectorProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_WindowList.clear(); EnumWindows(EnumWindowsProc, 0);
        g_hSelectorList = CreateWindow(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 10, 10, 360, 400, hWnd, (HMENU)101, (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
        CreateWindow(L"BUTTON", L"Select Target", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 150, 420, 100, 30, hWnd, (HMENU)IDOK, (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), NULL);
        for (const auto &w : g_WindowList) SendMessage(g_hSelectorList, LB_ADDSTRING, 0, (LPARAM)w.title.c_str());
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK || (LOWORD(wParam) == 101 && HIWORD(wParam) == LBN_DBLCLK)) {
            int idx = (int)SendMessage(g_hSelectorList, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR) { g_hSelectedWindow = g_WindowList[idx].hwnd; DestroyWindow(hWnd); }
        }
        break;
    }
    case WM_CLOSE: DestroyWindow(hWnd); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

HWND DoWindowPicker(HINSTANCE hInst) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, SelectorProc, 0, 0, hInst, NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, L"SelCls", NULL };
    RegisterClassEx(&wc);
    HWND hDlg = CreateWindow(L"SelCls", L"Overlay Target Picker", WS_VISIBLE | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 100, 100, 400, 500, NULL, NULL, hInst, NULL);
    MSG msg; while (IsWindow(hDlg) && GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return g_hSelectedWindow;
}

// =============================================================
// MAIN ENTRY
// =============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {

    // 1. Initialize Context
    if (FAILED(g_D3DContext.Initialize())) return -1;

    // 2. Pick Target
    HWND targetHwnd = DoWindowPicker(hInst);
    if (!targetHwnd) return 0;

    // 3. Init Overlay
    if (!g_Overlay.Initialize(&g_D3DContext)) {
        MessageBox(NULL, L"Failed to init overlay", L"Error", MB_ICONERROR);
        return -1;
    }
    if (!g_Overlay.BindToWindow(targetHwnd)) {
        MessageBox(NULL, L"Failed to bind overlay", L"Error", MB_ICONERROR);
        return -1;
    }

    // 4. Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // Init ImGui Backends
    // Note: We use the Overlay Window Handle for Win32 Init
    ImGui_ImplWin32_Init(g_Overlay.GetHandle());
    ImGui_ImplDX11_Init(g_D3DContext.GetDevice(), g_D3DContext.GetContext());

    // 5. Set Callback
    FRenderDelegate callback;
    callback.BindStatic(ImGuiRenderLoop);
    g_Overlay.SetRenderCallback(callback);

    FWindowProcedureDelegate wndProcCallback;
    wndProcCallback.BindStatic(OnOverlayWindowMessage);
    g_Overlay.SetWindowProcedureCallback(wndProcCallback);

    // 6. Main Loop
    MSG msg = {};
    while (g_Overlay.IsValid()) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            g_Overlay.Update();
            g_Overlay.Render();
        }
    }

    // 7. Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_D3DContext.Cleanup();

    return 0;
}