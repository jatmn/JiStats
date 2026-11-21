# JiStats - AI Usage Tracker
# CMake project configuration

cmake_minimum_required(VERSION 3.15)
project(JiStats VERSION 1.0.0 LANGUAGES C CXX)

include(CTest)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Project options
option(JISTATS_BUILD_TESTS "Build tests" OFF)
option(JISTATS_BUILD_EXAMPLES "Build examples" OFF)
option(JISTATS_USE_SYSTEM_SQLITE "Use system SQLite" OFF)
option(JISTATS_USE_SYSTEM_JSON "Use system JSON library" OFF)
option(JISTATS_ENABLE_UPX "Compress Release executables with an external unmodified UPX tool" ON)
set(JISTATS_UPX_EXECUTABLE "" CACHE FILEPATH "Path to an official unmodified UPX executable")

if(JISTATS_ENABLE_UPX)
    set(JISTATS_LOCAL_UPX_HINTS
        "${CMAKE_SOURCE_DIR}/tools/upx"
    )

    if(JISTATS_UPX_EXECUTABLE AND NOT EXISTS "${JISTATS_UPX_EXECUTABLE}")
        message(WARNING "Configured JISTATS_UPX_EXECUTABLE does not exist: ${JISTATS_UPX_EXECUTABLE}")
        set(JISTATS_UPX_EXECUTABLE "" CACHE FILEPATH "Path to an official unmodified UPX executable" FORCE)
    endif()

    if(NOT JISTATS_UPX_EXECUTABLE)
        set(JISTATS_LOCAL_UPX_EXE "")
        foreach(JISTATS_LOCAL_UPX_HINT IN LISTS JISTATS_LOCAL_UPX_HINTS)
            if(EXISTS "${JISTATS_LOCAL_UPX_HINT}/upx.exe")
                set(JISTATS_LOCAL_UPX_EXE "${JISTATS_LOCAL_UPX_HINT}/upx.exe")
                break()
            endif()
        endforeach()

        if(JISTATS_LOCAL_UPX_EXE)
            set(JISTATS_UPX_EXECUTABLE "${JISTATS_LOCAL_UPX_EXE}" CACHE FILEPATH "Path to an official unmodified UPX executable" FORCE)
        endif()
    endif()

    if(NOT JISTATS_UPX_EXECUTABLE)
        find_program(
            JISTATS_UPX_EXECUTABLE
            NAMES upx upx.exe
            HINTS ${JISTATS_LOCAL_UPX_HINTS}
            DOC "Path to an official unmodified UPX executable"
        )
    endif()

    if(JISTATS_UPX_EXECUTABLE)
        message(STATUS "UPX packing enabled with ${JISTATS_UPX_EXECUTABLE}")
    else()
        message(WARNING "JISTATS_ENABLE_UPX is ON but no unmodified UPX executable was found. Release binaries will not be packed.")
    endif()
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Find SQLite3
set(JISTATS_SQLITE_TARGET "")
if(JISTATS_USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)
    set(JISTATS_SQLITE_TARGET SQLite::SQLite3)
    add_compile_definitions(JISTATS_USE_SYSTEM_SQLITE=1)
else()
    set(JISTATS_BUNDLED_SQLITE_DIR ${CMAKE_SOURCE_DIR}/third_party/sqlite)
    if(EXISTS "${JISTATS_BUNDLED_SQLITE_DIR}/sqlite3.c" AND EXISTS "${JISTATS_BUNDLED_SQLITE_DIR}/sqlite3.h")
        add_library(jistats_sqlite STATIC
            ${JISTATS_BUNDLED_SQLITE_DIR}/sqlite3.c
        )
        target_include_directories(jistats_sqlite PUBLIC
            ${JISTATS_BUNDLED_SQLITE_DIR}
        )
        target_compile_definitions(jistats_sqlite PUBLIC JISTATS_HAS_SQLITE=1)
        if(MSVC)
            target_compile_options(jistats_sqlite PRIVATE /FS /W0)
        endif()
        set(JISTATS_SQLITE_TARGET jistats_sqlite)
    else()
        message(STATUS "Bundled SQLite amalgamation not found in third_party/sqlite/.")
    endif()
endif()

# Add ImGui (optional; current dashboard shell uses native Win32 controls)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp")
    add_subdirectory(third_party/imgui)
    set(JISTATS_HAS_IMGUI ON)
else()
    message(STATUS "ImGui source not found. Optional ImGui surfaces will be disabled.")
    message(STATUS "Run: git submodule add https://github.com/ocornut/imgui.git third_party/imgui")
    set(JISTATS_HAS_IMGUI OFF)
endif()

# Add spdlog (header-only logging library)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/spdlog/include/spdlog/spdlog.h")
    add_library(spdlog INTERFACE)
    target_include_directories(spdlog INTERFACE
        ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
    )
    set(JISTATS_HAS_SPDLOG ON)
else()
    message(STATUS "spdlog not found in third_party/spdlog/")
    set(JISTATS_HAS_SPDLOG OFF)
endif()

# Add subdirectories
add_subdirectory(src)

if(JISTATS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(JISTATS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation
install(
    DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

install(
    FILES
        ${CMAKE_SOURCE_DIR}/LICENSE
        ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION .
)

install(
    TARGETS JiStats
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
