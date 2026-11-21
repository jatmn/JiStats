# Dear ImGui CMake configuration
# This file expects ImGui source files to be present in this directory
# They can be obtained from https://github.com/ocornut/imgui

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Core ImGui source files
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)

# Win32 backend (for Windows native)
set(IMGUI_SOURCES_WIN32
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# Create ImGui library
add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_SOURCES_WIN32})

# Include directories
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# Windows specific settings
if(WIN32)
    # Link with Windows libraries for DirectX 11
    target_link_libraries(imgui PRIVATE
        d3d11
        dxgi
    )
    
    # Define Windows version
    target_compile_definitions(imgui PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# C++17 required
target_compile_features(imgui PUBLIC cxx_std_17)
