#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <stdio.h>
#include <iostream>
#include <aclapi.h>
#include <sddl.h>

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
#pragma comment(lib, "Advapi32.lib")

// =============================================================
// GLOBALS
// =============================================================
CD3D11Context     g_D3DContext;
CWindowOverlay    g_Overlay;
bool              g_RequestScreenshot = false;
bool              g_AntiCapture = false;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// =============================================================
// PROCESS PROTECTION
// =============================================================
bool EnablePrivilege(LPCWSTR lpszPrivilege)
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp;
    if (!LookupPrivilegeValue(NULL, lpszPrivilege, &tp.Privileges[0].Luid))
    {
        CloseHandle(hToken);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
    {
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(hToken);
    return GetLastError() == ERROR_SUCCESS;
}

bool ProtectCurrentProcess() {
    // 1. Enable SeSecurityPrivilege to set SACL
    if (!EnablePrivilege(SE_SECURITY_NAME)) {
        printf("Failed to enable SeSecurityPrivilege. Run as Admin.\n");
    }

    HANDLE hProcess = GetCurrentProcess();
    
    // Define SDDL:
    // D: DACL
    // (D;;GA;;;WD) -> Deny Generic All to World (Everyone)
    // (A;;GA;;;SY) -> Allow Generic All to System
    // (A;;GA;;;OW) -> Allow Generic All to Owner
    // S: SACL
    // (AU;SAFA;GA;;;WD) -> Audit Success/Failure for Generic All for World
    const wchar_t* sddl = L"D:(D;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;OW)S:(AU;SAFA;GA;;;WD)";

    PSECURITY_DESCRIPTOR pNewSD = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, 
            SDDL_REVISION_1, &pNewSD, NULL)) {
        printf("ConvertStringSecurityDescriptorToSecurityDescriptorW failed: %d\n", GetLastError());
        return false;
    }

    // Set both DACL and SACL
    SECURITY_INFORMATION secInfo = DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION;
    
    if (!SetKernelObjectSecurity(hProcess, secInfo, pNewSD)) {
        printf("SetKernelObjectSecurity failed: %d\n", GetLastError());
        LocalFree(pNewSD);
        return false;
    }

    LocalFree(pNewSD);
    printf("Process Protection & Auditing Enabled.\n");
    return true;
}

// =============================================================
// SCREENSHOT HELPER
// =============================================================
void CaptureScreenshot(HWND targetWindow)
{
    RECT rect;
    GetWindowRect(targetWindow, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    HDC hScreenDC = GetDC(NULL);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    // Copy screen content
    BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, rect.left, rect.top, SRCCOPY);

    // Save to file
    BITMAP bmpScreen;
    GetObject(hBitmap, sizeof(BITMAP), &bmpScreen);
    BITMAPFILEHEADER   bmfHeader;
    BITMAPINFOHEADER   bi;

    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bmpScreen.bmWidth;
    bi.biHeight = bmpScreen.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    DWORD dwBmpSize = ((bmpScreen.bmWidth * bi.biBitCount + 31) / 32) * 4 * bmpScreen.bmHeight;
    HANDLE hDIB = GlobalAlloc(GHND, dwBmpSize);
    char* lpbitmap = (char*)GlobalLock(hDIB);

    GetDIBits(hScreenDC, hBitmap, 0, (UINT)bmpScreen.bmHeight, lpbitmap, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    HANDLE hFile = CreateFile(L"screenshot.bmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD dwBytesWritten = 0;
    
    bmfHeader.bfType = 0x4D42; // BM
    bmfHeader.bfSize = dwBmpSize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmfHeader.bfReserved1 = 0;
    bmfHeader.bfReserved2 = 0;
    bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);

    WriteFile(hFile, (LPSTR)&bmfHeader, sizeof(BITMAPFILEHEADER), &dwBytesWritten, NULL);
    WriteFile(hFile, (LPSTR)&bi, sizeof(BITMAPINFOHEADER), &dwBytesWritten, NULL);
    WriteFile(hFile, (LPSTR)lpbitmap, dwBmpSize, &dwBytesWritten, NULL);

    CloseHandle(hFile);
    GlobalUnlock(hDIB);
    GlobalFree(hDIB);

    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

    printf("Screenshot saved to screenshot.bmp\n");
}

bool IsCursorOverImGui()
{
    if (ImGui::GetCurrentContext() == nullptr) return false;

    ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse) return true;

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup)) return true;

    return false;
}

bool g_state = 1;

HRESULT WindowProcedureCallback(HWND WindowHandle, UINT Message, WPARAM WParameter, LPARAM LParameter, LRESULT *OutResult)
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
            if (g_state)
            {
                *OutResult = HTCLIENT;
                return S_OK;
            }
            *OutResult = HTTRANSPARENT;
            return S_OK;

        case WM_SETCURSOR:
            if (g_state)
            {
                return S_FALSE;
            }
            break;
    }

    return S_FALSE;
}

bool CalculateVisibility(HWND TargetWindowHandle)
{
    HWND ForegroundWindow = GetForegroundWindow();

    if (IsIconic(TargetWindowHandle))
    {
        return false;
    }
    
    if (ForegroundWindow == TargetWindowHandle || ForegroundWindow == nullptr)
    {
        return true;
    }
    else
    {
        return false;
    }
        

    if (!IsWindow(TargetWindowHandle))
        return false;

    return true;
}

HHOOK g_hKeyboardHook = NULL;

ImGuiKey MyKeyEventToImGuiKey(WPARAM wParam, LPARAM lParam)
{
    if ((wParam == VK_RETURN) && (HIWORD(lParam) & KF_EXTENDED))
        return ImGuiKey_KeypadEnter;
    const int scancode = (int)LOBYTE(HIWORD(lParam));
    switch (wParam)
    {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_CAPITAL: return ImGuiKey_CapsLock;
        case VK_SCROLL: return ImGuiKey_ScrollLock;
        case VK_NUMLOCK: return ImGuiKey_NumLock;
        case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
        case VK_PAUSE: return ImGuiKey_Pause;
        case VK_NUMPAD0: return ImGuiKey_Keypad0;
        case VK_NUMPAD1: return ImGuiKey_Keypad1;
        case VK_NUMPAD2: return ImGuiKey_Keypad2;
        case VK_NUMPAD3: return ImGuiKey_Keypad3;
        case VK_NUMPAD4: return ImGuiKey_Keypad4;
        case VK_NUMPAD5: return ImGuiKey_Keypad5;
        case VK_NUMPAD6: return ImGuiKey_Keypad6;
        case VK_NUMPAD7: return ImGuiKey_Keypad7;
        case VK_NUMPAD8: return ImGuiKey_Keypad8;
        case VK_NUMPAD9: return ImGuiKey_Keypad9;
        case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case VK_ADD: return ImGuiKey_KeypadAdd;
        case VK_SHIFT: return ImGuiKey_LeftShift;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_CONTROL: return ImGuiKey_LeftCtrl;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_MENU: return ImGuiKey_LeftAlt;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        case VK_LWIN: return ImGuiKey_LeftSuper;
        case VK_RWIN: return ImGuiKey_RightSuper;
        case VK_APPS: return ImGuiKey_Menu;
        case '0': return ImGuiKey_0;
        case '1': return ImGuiKey_1;
        case '2': return ImGuiKey_2;
        case '3': return ImGuiKey_3;
        case '4': return ImGuiKey_4;
        case '5': return ImGuiKey_5;
        case '6': return ImGuiKey_6;
        case '7': return ImGuiKey_7;
        case '8': return ImGuiKey_8;
        case '9': return ImGuiKey_9;
        case 'A': return ImGuiKey_A;
        case 'B': return ImGuiKey_B;
        case 'C': return ImGuiKey_C;
        case 'D': return ImGuiKey_D;
        case 'E': return ImGuiKey_E;
        case 'F': return ImGuiKey_F;
        case 'G': return ImGuiKey_G;
        case 'H': return ImGuiKey_H;
        case 'I': return ImGuiKey_I;
        case 'J': return ImGuiKey_J;
        case 'K': return ImGuiKey_K;
        case 'L': return ImGuiKey_L;
        case 'M': return ImGuiKey_M;
        case 'N': return ImGuiKey_N;
        case 'O': return ImGuiKey_O;
        case 'P': return ImGuiKey_P;
        case 'Q': return ImGuiKey_Q;
        case 'R': return ImGuiKey_R;
        case 'S': return ImGuiKey_S;
        case 'T': return ImGuiKey_T;
        case 'U': return ImGuiKey_U;
        case 'V': return ImGuiKey_V;
        case 'W': return ImGuiKey_W;
        case 'X': return ImGuiKey_X;
        case 'Y': return ImGuiKey_Y;
        case 'Z': return ImGuiKey_Z;
        case VK_F1: return ImGuiKey_F1;
        case VK_F2: return ImGuiKey_F2;
        case VK_F3: return ImGuiKey_F3;
        case VK_F4: return ImGuiKey_F4;
        case VK_F5: return ImGuiKey_F5;
        case VK_F6: return ImGuiKey_F6;
        case VK_F7: return ImGuiKey_F7;
        case VK_F8: return ImGuiKey_F8;
        case VK_F9: return ImGuiKey_F9;
        case VK_F10: return ImGuiKey_F10;
        case VK_F11: return ImGuiKey_F11;
        case VK_F12: return ImGuiKey_F12;
        default: break;
    }
    return ImGuiKey_None;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;

        if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput)
        {
            // Only capture if the target window or overlay is foreground
            HWND fg = GetForegroundWindow();
            if (fg == g_Overlay.GetTargetWindow() || fg == g_Overlay.GetHandle())
            {
                ImGuiIO& io = ImGui::GetIO();
                
                bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

                // Track modifiers manually because GetAsyncKeyState is unreliable in the hook
                static bool s_LShift = false;
                static bool s_RShift = false;
                static bool s_LCtrl = false;
                static bool s_RCtrl = false;
                static bool s_LAlt = false;
                static bool s_RAlt = false;
                static bool s_LSuper = false;
                static bool s_RSuper = false;

                if (p->vkCode == VK_LSHIFT) s_LShift = isDown;
                if (p->vkCode == VK_RSHIFT) s_RShift = isDown;
                if (p->vkCode == VK_LCONTROL) s_LCtrl = isDown;
                if (p->vkCode == VK_RCONTROL) s_RCtrl = isDown;
                if (p->vkCode == VK_LMENU) s_LAlt = isDown;
                if (p->vkCode == VK_RMENU) s_RAlt = isDown;
                if (p->vkCode == VK_LWIN) s_LSuper = isDown;
                if (p->vkCode == VK_RWIN) s_RSuper = isDown;

                io.AddKeyEvent(ImGuiMod_Ctrl, s_LCtrl || s_RCtrl);
                io.AddKeyEvent(ImGuiMod_Shift, s_LShift || s_RShift);
                io.AddKeyEvent(ImGuiMod_Alt, s_LAlt || s_RAlt);
                io.AddKeyEvent(ImGuiMod_Super, s_LSuper || s_RSuper);

                // printf("Hook: VK=%d Down=%d | Mods: C=%d S=%d A=%d\n", p->vkCode, isDown, s_LCtrl||s_RCtrl, s_LShift||s_RShift, s_LAlt||s_RAlt);

                if (isDown)
                {
                    // Construct fake lParam for key mapping
                    LPARAM fakeLParam = (p->scanCode << 16) | ((p->flags & LLKHF_EXTENDED) ? (1 << 24) : 0);
                    ImGuiKey key = MyKeyEventToImGuiKey(p->vkCode, fakeLParam);
                    if (key != ImGuiKey_None)
                        io.AddKeyEvent(key, true);

                    // Handle character input
                    BYTE keyState[256] = {};
                    // Populate keyState with our manual modifiers
                    if (s_LShift || s_RShift) keyState[VK_SHIFT] = 0x80;
                    if (s_LCtrl || s_RCtrl) keyState[VK_CONTROL] = 0x80;
                    if (s_LAlt || s_RAlt) keyState[VK_MENU] = 0x80;
                    
                    // Also set the specific keys
                    if (s_LShift) keyState[VK_LSHIFT] = 0x80;
                    if (s_RShift) keyState[VK_RSHIFT] = 0x80;
                    if (s_LCtrl) keyState[VK_LCONTROL] = 0x80;
                    // ... etc

                    // Ensure current key is marked down in state
                    keyState[p->vkCode] = 0x80;

                    WCHAR buffer[2] = {};
                    HKL layout = GetKeyboardLayout(0);
                    if (ToUnicodeEx(p->vkCode, p->scanCode, keyState, buffer, 2, 0, layout) == 1)
                    {
                        // Filter out non-printable characters (like Ctrl+A -> \x01)
                        if (buffer[0] >= 32) 
                            io.AddInputCharacter(buffer[0]);
                    }
                }
                else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                {
                    LPARAM fakeLParam = (p->scanCode << 16) | ((p->flags & LLKHF_EXTENDED) ? (1 << 24) : 0);
                    ImGuiKey key = MyKeyEventToImGuiKey(p->vkCode, fakeLParam);
                    if (key != ImGuiKey_None)
                        io.AddKeyEvent(key, false);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// =============================================================
// RENDER CALLBACK (IMGUI)
// =============================================================
void ImGuiRenderLoop(ID3D11DeviceContext *DeviceContext, ID3D11RenderTargetView *RenderTargetView)
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Manually update inputs AFTER backend NewFrame to override any incorrect state
    // due to the window being inactive (WS_EX_NOACTIVATE).
    ImGuiIO &io = ImGui::GetIO();
    
    POINT p;
    if (GetCursorPos(&p))
    {
        ScreenToClient(g_Overlay.GetHandle(), &p);
        io.MousePos = ImVec2((float)p.x, (float)p.y);
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

    // Update global state for WM_NCHITTEST
    // This is CRITICAL because when we return HTTRANSPARENT, we stop receiving mouse messages,
    // so WindowProcedureCallback won't update g_state. We must update it here (per frame).
    g_state = IsCursorOverImGui();

    // Fix cursor visibility: Only let ImGui control cursor if we are hovering ImGui
    // This prevents ImGui from forcing the cursor to be visible when playing the game
    bool overImGui = g_state;
    
    // Dynamically toggle WS_EX_TRANSPARENT based on whether we are over ImGui
    // This ensures that when we are NOT over ImGui, the window is truly transparent to input
    // and the game receives all mouse events (including raw input).
    g_Overlay.SetInputPassThrough(!overImGui);

    if (overImGui)
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        // Ensure cursor is visible when over ImGui
        while (ShowCursor(TRUE) < 0);
    }
    else
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        // Force hide cursor when not over ImGui
        while (ShowCursor(FALSE) >= 0);
    }

    // Manually update modifiers because GetKeyState used by ImGui_ImplWin32_NewFrame 
    // might not be accurate when window is not active.
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

    ImGui::NewFrame();

    {
        ImGui::Begin("SpyX Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::Text("Target Window Detected!");
        ImGui::Separator();

        // --- TEST 1: PRZYCISK (Sprawdza klikni�cie myszk�) ---
        static int clickCount = 0;
        if (ImGui::Button("Click Me Test"))
        {
            clickCount++;
        }
        ImGui::SameLine();
        ImGui::Text("Clicks: %d", clickCount);
        ImGui::Text("State: %d", g_state);

        if (ImGui::Button("Take Screenshot"))
        {
            g_RequestScreenshot = true;
        }

        if (ImGui::Checkbox("Anti-Screen Capture", &g_AntiCapture))
        {
            g_Overlay.SetContentProtection(g_AntiCapture);
        }

        ImGui::Text("Modifiers (ImGui): Ctrl:%d Shift:%d Alt:%d", io.KeyCtrl, io.KeyShift, io.KeyAlt);
        ImGui::Text("Modifiers (Async): Ctrl:%d Shift:%d Alt:%d", 
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

        // --- TEST 2: POLE TEKSTOWE (Sprawdza input klawiatury) ---
        // Uwaga: Je�li u�ywamy MA_NOACTIVATE, pisanie mo�e wymaga� dodatkowej logiki,
        // ale to pole pozwoli sprawdzi�, czy ImGui w og�le dostaje focus.
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

    // 0. Protect Process (Anti-Cheat Evasion Attempt)
    if (ProtectCurrentProcess()) {
        // Protection applied
    }

    // Create Console for Debugging
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    printf("Debug Console Started\n");

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

    // Enable VSync to reduce GPU usage and prevent stutter
    g_Overlay.SetSwapInterval(0);
    
    // 4. Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // Load Font with Polish characters support
    // Range 0x0020-0x00FF (Basic Latin + Latin Supplement)
    // Range 0x0100-0x024F (Latin Extended-A + B) - Covers Polish characters like ą, ć, ę, ł, ń, ó, ś, ź, ż
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0100, 0x024F, 0 };
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, NULL, ranges);
    if (font == nullptr)
    {
        // Fallback to Arial if Segoe UI is missing
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f, NULL, ranges);
    }

    // Init ImGui Backends
    // Note: We use the Overlay Window Handle for Win32 Init
    ImGui_ImplWin32_Init(g_Overlay.GetHandle());
    ImGui_ImplDX11_Init(g_D3DContext.GetDevice(), g_D3DContext.GetContext());

    // 5. Set Callback
    FRenderDelegate callback;
    callback.BindStatic(ImGuiRenderLoop);
    g_Overlay.SetRenderCallback(callback);

    FWindowProcedureDelegate wndProcCallback;
    wndProcCallback.BindStatic(WindowProcedureCallback);
    g_Overlay.SetWindowProcedureCallback(wndProcCallback);
    
    
    // 6. Main Loop
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);

    MSG msg = {};
    while (g_Overlay.IsValid()) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        if (g_RequestScreenshot)
        {
            g_RequestScreenshot = false;
            
            // If Anti-Capture is ON, we need to temporarily disable it to capture the overlay
            if (g_AntiCapture)
            {
                g_Overlay.SetContentProtection(false);
                Sleep(50); // Wait for DWM update
            }

            // Capture
            CaptureScreenshot(g_Overlay.GetTargetWindow());

            // Restore Anti-Capture if it was ON
            if (g_AntiCapture)
            {
                g_Overlay.SetContentProtection(true);
            }
        }

        bool Visibility = CalculateVisibility(targetHwnd);
        if (g_Overlay.GetOverlayVisibility() != Visibility) {
            g_Overlay.SetOverlayVisibility(Visibility);
        }

        g_Overlay.Update();
        g_Overlay.Render();

        // Simple frame limiter to reduce CPU usage and prevent game lag
        // Target ~144 FPS (approx 7ms per frame)
        // Since the game likely sets high timer resolution, Sleep(1) is effective.
        //Sleep(1); 
    }

    if (g_hKeyboardHook) UnhookWindowsHookEx(g_hKeyboardHook);

    // 7. Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_D3DContext.Cleanup();

    return 0;
}