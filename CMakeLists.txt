# JiStats - AI Usage Tracker
# CMake project configuration

cmake_minimum_required(VERSION 3.20)
project(JiStats VERSION 1.0.0 LANGUAGES C CXX)

include(CTest)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Project options
option(JISTATS_BUILD_TESTS "Build tests" OFF)
option(JISTATS_BUILD_MINIMAX_CLI "Build the standalone MiniMax developer CLI" OFF)
option(JISTATS_USE_SYSTEM_SQLITE "Use system SQLite" OFF)
option(JISTATS_ENABLE_UPX "Compress Release executables with an external unmodified UPX tool" ON)
set(JISTATS_UPX_EXECUTABLE "" CACHE FILEPATH "Path to an official unmodified UPX executable")

if(WIN32 AND JISTATS_ENABLE_UPX)
    if(JISTATS_UPX_EXECUTABLE AND NOT EXISTS "${JISTATS_UPX_EXECUTABLE}")
        message(WARNING "Configured JISTATS_UPX_EXECUTABLE does not exist: ${JISTATS_UPX_EXECUTABLE}")
        set(JISTATS_UPX_EXECUTABLE "" CACHE FILEPATH "Path to an official unmodified UPX executable" FORCE)
    endif()

    if(NOT JISTATS_UPX_EXECUTABLE)
        # Search for a host tool separately from the cached user override.
        find_program(JISTATS_UPX_FROM_PATH NAMES upx NO_CMAKE_FIND_ROOT_PATH)
        if(JISTATS_UPX_FROM_PATH)
            set(JISTATS_UPX_EXECUTABLE "${JISTATS_UPX_FROM_PATH}")
        endif()
        unset(JISTATS_UPX_FROM_PATH CACHE)
    endif()

    if(JISTATS_UPX_EXECUTABLE)
        message(STATUS "UPX packing enabled with ${JISTATS_UPX_EXECUTABLE}")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release" OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
        message(WARNING "JISTATS_ENABLE_UPX is ON but no unmodified UPX executable was found. Release binaries will not be packed.")
    endif()
endif()

# Link the MinGW runtime statically. Explicit import libraries can still add
# DLL dependencies.
if(MINGW)
    add_link_options(-static)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Embed the baseline and upgrades for portable launches outside the source tree.
set(JISTATS_BASELINE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/012_baseline_schema.sql")
set(JISTATS_CLAUDE_REDACTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/013_redact_claude_source_references.sql")
set(JISTATS_CHATGPT_DEDUP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/014_deduplicate_chatgpt_activity.sql")
set(JISTATS_QUOTA_PRECISION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/015_quota_snapshot_precision.sql")
set(JISTATS_CLAUDE_ACTIVITY_DEDUP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/016_deduplicate_claude_daily_activity.sql")
set(JISTATS_KIMI_TASK_DEDUP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/db_migrations/017_kimi_task_activity_identity.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${JISTATS_BASELINE_FILE}"
    "${JISTATS_CLAUDE_REDACTION_FILE}"
    "${JISTATS_CHATGPT_DEDUP_FILE}"
    "${JISTATS_QUOTA_PRECISION_FILE}"
    "${JISTATS_CLAUDE_ACTIVITY_DEDUP_FILE}"
    "${JISTATS_KIMI_TASK_DEDUP_FILE}"
)
file(READ "${JISTATS_BASELINE_FILE}" JISTATS_BASELINE_SQL)
file(READ "${JISTATS_CLAUDE_REDACTION_FILE}" JISTATS_CLAUDE_REDACTION_SQL)
file(READ "${JISTATS_CHATGPT_DEDUP_FILE}" JISTATS_CHATGPT_DEDUP_SQL)
file(READ "${JISTATS_QUOTA_PRECISION_FILE}" JISTATS_QUOTA_PRECISION_SQL)
file(READ "${JISTATS_CLAUDE_ACTIVITY_DEDUP_FILE}" JISTATS_CLAUDE_ACTIVITY_DEDUP_SQL)
file(READ "${JISTATS_KIMI_TASK_DEDUP_FILE}" JISTATS_KIMI_TASK_DEDUP_SQL)
# Keep individual literals within MSVC's size limit.
string(REPLACE "\n" "\n)jistats_sql\"\n    R\"jistats_sql(" JISTATS_BASELINE_SQL "${JISTATS_BASELINE_SQL}")
configure_file(src/db/BaselineSchema.hpp.in generated/BaselineSchema.hpp @ONLY)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

include(cmake/ThirdPartySources.cmake)

# Find SQLite3
set(JISTATS_SQLITE_TARGET "")
if(JISTATS_USE_SYSTEM_SQLITE)
    if(MINGW AND CMAKE_CROSSCOMPILING)
        find_package(SQLite3 QUIET)
        if(NOT SQLite3_FOUND)
            message(FATAL_ERROR "SQLite3 for the MinGW-w64 target was not found. Install it in the target prefix or use bundled SQLite with -DJISTATS_USE_SYSTEM_SQLITE=OFF.")
        endif()
    else()
        find_package(SQLite3 REQUIRED)
    endif()
    set(JISTATS_SQLITE_TARGET SQLite::SQLite3)
    add_compile_definitions(JISTATS_USE_SYSTEM_SQLITE=1)
else()
    FetchContent_MakeAvailable(sqlite)
    add_library(jistats_sqlite STATIC
        ${sqlite_SOURCE_DIR}/sqlite3.c
    )
    target_include_directories(jistats_sqlite PUBLIC
        ${sqlite_SOURCE_DIR}
    )
    target_compile_definitions(jistats_sqlite PUBLIC JISTATS_HAS_SQLITE=1)
    if(MSVC)
        target_compile_options(jistats_sqlite PRIVATE /FS /W0)
    endif()
    set(JISTATS_SQLITE_TARGET jistats_sqlite)
endif()

# Add spdlog (header-only logging library)
FetchContent_MakeAvailable(spdlog)
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE
    ${spdlog_SOURCE_DIR}/include
)
set(JISTATS_HAS_SPDLOG ON)

if(JISTATS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(NOT WIN32)
    if(NOT JISTATS_BUILD_TESTS)
        message(FATAL_ERROR "JiStats targets Windows. On Linux, set JISTATS_BUILD_TESTS=ON for portable tests or use cmake/toolchains/mingw-w64-x86_64.cmake for Windows binaries.")
    endif()
    return()
endif()

add_subdirectory(src)

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
