cmake_minimum_required(VERSION 3.20)
# Download and verify the same archives as the build without probing compilers.
project(JiStatsSourcePrefetch LANGUAGES NONE)

include(../ThirdPartySources.cmake)
FetchContent_MakeAvailable(sqlite spdlog catch2)
