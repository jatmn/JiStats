# Dear ImGui Source Files

This directory should contain the Dear ImGui source files.

## How to obtain ImGui

1. Download from GitHub: https://github.com/ocornut/imgui
2. Or use git submodule:
   ```
   git submodule add https://github.com/ocornut/imgui.git third_party/imgui/src
   ```

## Required Files

The following files should be present:
- imgui.cpp
- imgui.h
- imgui_demo.cpp
- imgui_draw.cpp
- imgui_tables.cpp
- imgui_widgets.cpp
- imconfig.h
- imgui_internal.h
- imstb_rectpack.h
- imstb_textedit.h
- imstb_truetype.h

## Backend Files (for Win32 + DirectX 11)

The following backend files should be in the `backends/` subdirectory:
- imgui_impl_win32.cpp
- imgui_impl_win32.h
- imgui_impl_dx11.cpp
- imgui_impl_dx11.h

## Alternative: Use FetchContent

You can also use CMake's FetchContent to download ImGui automatically.
See the main CMakeLists.txt for configuration options.
