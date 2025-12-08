// Ten plik s³u¿y tylko do skompilowania biblioteki ImGui.
// Dodaj TYLKO ten plik do swojego projektu w Visual Studio.

// Wy³¹czamy ostrze¿enia dla zewnêtrznej biblioteki, ¿eby nie œmieci³y w konsoli
#pragma warning(push, 0)

// Core ImGui
#include "imgui.cpp"
#include "imgui_draw.cpp"
#include "imgui_tables.cpp"
#include "imgui_widgets.cpp"

// Demo (opcjonalne, ale przydatne)
#include "imgui_demo.cpp"

// Backends (Dostosuj œcie¿kê jeœli s¹ w podfolderze 'backends')
// Czêsto s¹ w: imgui/backends/imgui_impl_win32.cpp
#include "imgui_impl_win32.cpp"
#include "imgui_impl_dx11.cpp"

#pragma warning(pop)