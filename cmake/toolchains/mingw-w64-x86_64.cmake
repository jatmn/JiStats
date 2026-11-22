# Cross-compile the Windows x64 binaries from Linux with MinGW-w64 GCC.
#
#   cmake -S . -B build-mingw -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
#
# Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix imagemagick ninja-build cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(JISTATS_MINGW_TRIPLE x86_64-w64-mingw32)

# Prefer the posix thread model, which provides std::thread on every GCC version.
find_program(CMAKE_C_COMPILER NAMES ${JISTATS_MINGW_TRIPLE}-gcc-posix ${JISTATS_MINGW_TRIPLE}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${JISTATS_MINGW_TRIPLE}-g++-posix ${JISTATS_MINGW_TRIPLE}-g++)
find_program(CMAKE_RC_COMPILER NAMES ${JISTATS_MINGW_TRIPLE}-windres)
foreach(tool CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_RC_COMPILER)
    if(NOT ${tool})
        message(FATAL_ERROR "MinGW-w64 (${JISTATS_MINGW_TRIPLE}) compiler tools were not found on PATH.")
    endif()
endforeach()

# Search the target environment for libraries and headers, and the host for tools.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
