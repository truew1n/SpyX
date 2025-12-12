#pragma once

#ifdef SPYX_EXPORTS
#define SPYX_API __declspec(dllexport)
#else
#define SPYX_API __declspec(dllimport)
#endif

extern "C" {
    // Initialization
    SPYX_API bool SpyX_Initialize();
    SPYX_API bool SpyX_BindToWindow(const char* windowName);
    SPYX_API void SpyX_Cleanup();

    // Frame Control
    SPYX_API void SpyX_BeginFrame();
    SPYX_API void SpyX_EndFrame();

    // Drawing Primitives (Add more as needed)
    SPYX_API void SpyX_DrawBox(float x, float y, float w, float h, float r, float g, float b, float a, float thickness);
    SPYX_API void SpyX_DrawFilledBox(float x, float y, float w, float h, float r, float g, float b, float a);
    SPYX_API void SpyX_DrawText(float x, float y, const char* text, float r, float g, float b, float a, float fontSize);
    SPYX_API void SpyX_DrawLine(float x1, float y1, float x2, float y2, float r, float g, float b, float a, float thickness);

    // ImGui Interactive Widgets
    SPYX_API bool SpyX_Begin(const char* name, bool* p_open, int flags);
    SPYX_API void SpyX_End();
    
    SPYX_API bool SpyX_Button(const char* label, float w, float h);
    SPYX_API bool SpyX_Checkbox(const char* label, bool* v);
    SPYX_API bool SpyX_InputText(const char* label, char* buf, size_t buf_size);
    SPYX_API bool SpyX_SliderFloat(const char* label, float* v, float v_min, float v_max);
    SPYX_API bool SpyX_ColorEdit4(const char* label, float* col);
    
    SPYX_API void SpyX_SameLine(float offset_from_start_x, float spacing);
    SPYX_API void SpyX_Separator();
    SPYX_API void SpyX_Text(const char* text);

    // Window Layout
    SPYX_API void SpyX_SetNextWindowPos(float x, float y, int cond);
    SPYX_API void SpyX_SetNextWindowSize(float w, float h, int cond);
    SPYX_API void SpyX_SetNextWindowBgAlpha(float alpha);
}
