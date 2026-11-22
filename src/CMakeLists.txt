# JiStats source CMake configuration

# Source files for the main application (only include files that exist)
set(JISTATS_SOURCES
    app/main.cpp
    app/AppController.cpp
    app/TrayController.cpp
    observation/LocalObservationService.cpp
    observation/ObservationPersistenceBridge.cpp
    platform/StartupManager.cpp
    platform/WindowsAppAssets.cpp
    platform/WindowsCredentialStore.cpp
    db/StorageService.cpp
    db/SchemaManager.cpp
    db/SettingsService.cpp
    db/QueryResult.cpp
    providers/ProviderManager.cpp
    providers/minimax/MiniMaxAdapter.cpp
    providers/minimax/MiniMaxLiveCapture.cpp
    providers/openai/OpenAIAdapter.cpp
    providers/openai/OpenAIExportImport.cpp
    providers/openai/CodexLocalSqliteReader.cpp
    providers/openai/CodexLocalUsageImporter.cpp
    providers/claude/ClaudeCodeAdapter.cpp
    providers/claude/ClaudeCodeLocalParser.cpp
    analytics/MirrorEngine.cpp
    analytics/AnalyticsEngine.cpp
    analytics/GapClassifier.cpp
    gui/DashboardPresenter.cpp
    gui/AnalysisPresenter.cpp
    gui/NotificationService.cpp
    reporting/ReportingService.cpp
    reporting/HtmlExporter.cpp
    reporting/AnalysisQueryEngine.cpp
    taskbar/TaskbarPresenter.cpp
    core/RuntimeStatus.cpp
    core/Logger.cpp
    core/SyncScheduler.cpp
    core/HttpClient.cpp
    core/JsonParser.cpp
)

# Header files
set(JISTATS_HEADERS
    include/app/AppController.hpp
    include/app/TrayController.hpp
    include/observation/ActivityEvent.hpp
    include/observation/LocalObservationService.hpp
    include/observation/ObservationPersistenceBridge.hpp
    include/platform/StartupManager.hpp
    include/platform/WindowsAppAssets.hpp
    include/platform/WindowsCredentialStore.hpp
    include/db/StorageService.hpp
    include/db/SchemaManager.hpp
    include/db/SettingsService.hpp
    include/db/QueryResult.hpp
    include/providers/ProviderManager.hpp
    include/providers/IProviderAdapter.hpp
    include/providers/minimax/MiniMaxAdapter.hpp
    include/providers/minimax/MiniMaxLiveCapture.hpp
    include/providers/openai/OpenAIAdapter.hpp
    include/providers/openai/OpenAIExportImport.hpp
    include/providers/openai/CodexLocalSqliteReader.hpp
    include/providers/openai/CodexLocalUsageImporter.hpp
    include/providers/claude/ClaudeCodeAdapter.hpp
    include/providers/claude/ClaudeCodeLocalParser.hpp
    include/analytics/MirrorEngine.hpp
    include/analytics/AnalyticsEngine.hpp
    include/analytics/GapClassifier.hpp
    include/gui/DashboardPresenter.hpp
    include/gui/NotificationService.hpp
    include/reporting/ReportingService.hpp
    include/reporting/HtmlExporter.hpp
    include/taskbar/TaskbarPresenter.hpp
    include/core/RuntimeStatus.hpp
    include/core/SyncScheduler.hpp
)

set(JISTATS_KIMI_SOURCE ${CMAKE_SOURCE_DIR}/src/providers/kimi/KimiAdapter.cpp)
set(JISTATS_KIMI_HEADER ${CMAKE_SOURCE_DIR}/include/providers/kimi/KimiAdapter.hpp)

if(EXISTS ${JISTATS_KIMI_SOURCE})
    list(APPEND JISTATS_SOURCES providers/kimi/KimiAdapter.cpp)
endif()

if(EXISTS ${JISTATS_KIMI_HEADER})
    list(APPEND JISTATS_HEADERS include/providers/kimi/KimiAdapter.hpp)
endif()

# Resource files for Windows
if(WIN32)
    set(JISTATS_GENERATED_ICON ${CMAKE_CURRENT_BINARY_DIR}/jistats-app.ico)
    set(JISTATS_ICON_SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/generate_jistats_ico.ps1)
    set(JISTATS_RESOURCE_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/jistats_resource_paths.h)

    file(GENERATE
        OUTPUT ${JISTATS_ICON_SCRIPT}
        CONTENT [===[
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPng,
    [Parameter(Mandatory = $true)]
    [string]$OutputIco
)

Add-Type -AssemblyName System.Drawing

$directory = Split-Path -Parent $OutputIco
if ($directory) {
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
}

$bitmap = [System.Drawing.Bitmap]::FromFile($InputPng)
try {
    $maxDimension = [Math]::Max($bitmap.Width, $bitmap.Height)
    if ($maxDimension -le 0) {
        throw "Failed to read PNG dimensions from $InputPng"
    }

    $minX = $bitmap.Width
    $minY = $bitmap.Height
    $maxX = -1
    $maxY = -1
    for ($y = 0; $y -lt $bitmap.Height; $y++) {
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            $pixel = $bitmap.GetPixel($x, $y)
            if ($pixel.A -le 16) {
                continue
            }
            if ($x -lt $minX) { $minX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }

    if ($maxX -lt 0 -or $maxY -lt 0) {
        $minX = 0
        $minY = 0
        $maxX = $bitmap.Width - 1
        $maxY = $bitmap.Height - 1
    }

    $contentWidth = $maxX - $minX + 1
    $contentHeight = $maxY - $minY + 1
    $squareSize = [Math]::Max($contentWidth, $contentHeight)
    $sizes = @(16, 24, 32, 48, 64, 128, 256) | Where-Object { $_ -le [Math]::Max(256, $maxDimension) }
    $images = New-Object System.Collections.Generic.List[object]

    foreach ($size in $sizes) {
        $scaled = New-Object System.Drawing.Bitmap $size, $size
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($scaled)
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $inset = [Math]::Max(1, [Math]::Floor($size / 12))
                $targetSize = [Math]::Max(1, $size - ($inset * 2))
                $sourceX = $minX - (($squareSize - $contentWidth) / 2.0)
                $sourceY = $minY - (($squareSize - $contentHeight) / 2.0)
                $graphics.DrawImage(
                    $bitmap,
                    [System.Drawing.Rectangle]::new($inset, $inset, $targetSize, $targetSize),
                    [float]$sourceX,
                    [float]$sourceY,
                    [float]$squareSize,
                    [float]$squareSize,
                    [System.Drawing.GraphicsUnit]::Pixel
                )
            } finally {
                $graphics.Dispose()
            }

            $memory = New-Object System.IO.MemoryStream
            try {
                $scaled.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
                $images.Add([PSCustomObject]@{
                    Size = $size
                    Bytes = $memory.ToArray()
                }) | Out-Null
            } finally {
                $memory.Dispose()
            }
        } finally {
            $scaled.Dispose()
        }
    }

    $stream = [System.IO.File]::Open($OutputIco, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $writer = New-Object System.IO.BinaryWriter($stream)
        try {
            $writer.Write([UInt16]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]$images.Count)

            $offset = 6 + (16 * $images.Count)
            foreach ($image in $images) {
                $dimensionByte = if ($image.Size -ge 256) { [byte]0 } else { [byte]$image.Size }
                $writer.Write($dimensionByte)
                $writer.Write($dimensionByte)
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([UInt16]1)
                $writer.Write([UInt16]32)
                $writer.Write([UInt32]$image.Bytes.Length)
                $writer.Write([UInt32]$offset)
                $offset += $image.Bytes.Length
            }

            foreach ($image in $images) {
                $writer.Write($image.Bytes)
            }
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
} finally {
    $bitmap.Dispose()
}
]===]
    )

    if(CMAKE_HOST_WIN32)
        add_custom_command(
            OUTPUT ${JISTATS_GENERATED_ICON}
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File ${JISTATS_ICON_SCRIPT}
                -InputPng ${CMAKE_SOURCE_DIR}/assets/icon2.png
                -OutputIco ${JISTATS_GENERATED_ICON}
            DEPENDS
                ${CMAKE_SOURCE_DIR}/assets/icon2.png
                ${JISTATS_ICON_SCRIPT}
            VERBATIM
        )
        file(TO_NATIVE_PATH "${JISTATS_GENERATED_ICON}" JISTATS_GENERATED_ICON_NATIVE)
    else()
        # Cross-compiling from Linux or macOS: System.Drawing is Windows-only, so
        # build the same icon sizes with ImageMagick instead.
        find_program(JISTATS_IMAGEMAGICK_EXECUTABLE NAMES magick convert)
        if(NOT JISTATS_IMAGEMAGICK_EXECUTABLE)
            message(FATAL_ERROR "Building the Windows app icon on a non-Windows host requires ImageMagick (magick or convert).")
        endif()
        add_custom_command(
            OUTPUT ${JISTATS_GENERATED_ICON}
            COMMAND "${CMAKE_COMMAND}"
                "-DJISTATS_IMAGEMAGICK_EXECUTABLE=${JISTATS_IMAGEMAGICK_EXECUTABLE}"
                "-DJISTATS_ICON_INPUT=${CMAKE_SOURCE_DIR}/assets/icon2.png"
                "-DJISTATS_ICON_OUTPUT=${JISTATS_GENERATED_ICON}"
                -P "${CMAKE_SOURCE_DIR}/cmake/GenerateAppIcon.cmake"
            DEPENDS
                ${CMAKE_SOURCE_DIR}/assets/icon2.png
                ${CMAKE_SOURCE_DIR}/cmake/GenerateAppIcon.cmake
            VERBATIM
        )
        # The resource header needs a file path without POSIX shell escaping.
        set(JISTATS_GENERATED_ICON_NATIVE "${JISTATS_GENERATED_ICON}")
    endif()

    string(REPLACE "\\" "\\\\" JISTATS_GENERATED_ICON_ESCAPED "${JISTATS_GENERATED_ICON_NATIVE}")
    file(GENERATE
        OUTPUT ${JISTATS_RESOURCE_INCLUDE}
        CONTENT "#pragma once\n#define JISTATS_APP_ICON_PATH \"${JISTATS_GENERATED_ICON_ESCAPED}\"\n"
    )

    set(JISTATS_RESOURCES
        ${CMAKE_SOURCE_DIR}/resources/JiStats.rc
        ${JISTATS_GENERATED_ICON}
    )

    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/resources/JiStats.rc
        PROPERTIES
            OBJECT_DEPENDS "${JISTATS_GENERATED_ICON}"
    )
endif()

# Add executable
if(WIN32)
    add_executable(JiStats WIN32 ${JISTATS_SOURCES} ${JISTATS_RESOURCES})
else()
    add_executable(JiStats ${JISTATS_SOURCES})
endif()

# Target properties
set_target_properties(JiStats PROPERTIES
    OUTPUT_NAME "JiStats"
)
# VERSION sets the PE image version on Windows hosts. CMake on other hosts would
# instead name the file JiStats.exe-<version> behind a symlink.
if(CMAKE_HOST_WIN32)
    set_target_properties(JiStats PROPERTIES VERSION ${PROJECT_VERSION})
elseif(MINGW)
    # CMake emits a default 0.0 image version after LINK_OPTIONS. Place the
    # release version after that default without creating a versioned symlink.
    target_link_libraries(JiStats PRIVATE
        "-Wl,--major-image-version,${PROJECT_VERSION_MAJOR},--minor-image-version,${PROJECT_VERSION_MINOR}"
    )
endif()

# Include directories
target_include_directories(JiStats PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_BINARY_DIR}>
)

if(JISTATS_SQLITE_TARGET)
    target_link_libraries(JiStats PRIVATE ${JISTATS_SQLITE_TARGET})
endif()

# Link spdlog if available
if(JISTATS_HAS_SPDLOG)
    target_link_libraries(JiStats PRIVATE spdlog)
    target_compile_definitions(JiStats PRIVATE JISTATS_HAS_SPDLOG=1)
endif()

# Windows specific settings
if(WIN32)
    set_target_properties(JiStats PROPERTIES
        WIN32_EXECUTABLE ON
    )
    target_link_libraries(JiStats PRIVATE
        comctl32
        comdlg32
        gdiplus
        user32
        gdi32
        shell32
        advapi32
        winhttp
    )
    target_compile_definitions(JiStats PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        set_target_properties(JiStats PROPERTIES LINK_FLAGS "/MANIFEST:NO")
        target_compile_options(JiStats PRIVATE /utf-8 /W4 /EHsc /FS)
    elseif(MINGW)
        # MSVC links Shlwapi through #pragma comment; MinGW ignores that pragma.
        target_link_libraries(JiStats PRIVATE shlwapi)
        # Use the wWinMain entry point that MSVC selects automatically.
        target_link_options(JiStats PRIVATE -municode)
        target_sources(JiStats PRIVATE platform/MinGwCompat.cpp)
    endif()
endif()

function(jistats_enable_upx_for_target target_name)
    if(NOT JISTATS_ENABLE_UPX)
        return()
    endif()

    if(NOT JISTATS_UPX_EXECUTABLE)
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DJISTATS_UPX_EXECUTABLE=${JISTATS_UPX_EXECUTABLE}"
            "-DJISTATS_UPX_TARGET=$<TARGET_FILE:${target_name}>"
            "-DJISTATS_UPX_OUTPUT=$<TARGET_FILE_DIR:${target_name}>/$<TARGET_FILE_BASE_NAME:${target_name}>-upx$<TARGET_FILE_SUFFIX:${target_name}>"
            "-DJISTATS_UPX_TARGET_NAME=${target_name}"
            "-DJISTATS_UPX_CONFIG=$<CONFIG>"
        -P "${CMAKE_SOURCE_DIR}/cmake/RunUpx.cmake"
        COMMENT "Building ${target_name}-upx variant in Release builds"
        VERBATIM
    )
endfunction()

jistats_enable_upx_for_target(JiStats)

# Installation
install(TARGETS JiStats)

if(JISTATS_BUILD_MINIMAX_CLI)
    add_executable(JiStatsMiniMaxCli
        app/MiniMaxCliMain.cpp
        observation/LocalObservationService.cpp
        platform/WindowsCredentialStore.cpp
        db/StorageService.cpp
        db/SchemaManager.cpp
        db/QueryResult.cpp
        providers/minimax/MiniMaxAdapter.cpp
        providers/minimax/MiniMaxLiveCapture.cpp
        analytics/GapClassifier.cpp
        core/Logger.cpp
        core/HttpClient.cpp
        core/JsonParser.cpp
    )
    target_include_directories(JiStatsMiniMaxCli PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    if(JISTATS_SQLITE_TARGET)
        target_link_libraries(JiStatsMiniMaxCli PRIVATE ${JISTATS_SQLITE_TARGET})
    endif()

    if(JISTATS_HAS_SPDLOG)
        target_link_libraries(JiStatsMiniMaxCli PRIVATE spdlog)
        target_compile_definitions(JiStatsMiniMaxCli PRIVATE JISTATS_HAS_SPDLOG=1)
    endif()

    target_link_libraries(JiStatsMiniMaxCli PRIVATE
        advapi32
        winhttp
    )
    target_compile_definitions(JiStatsMiniMaxCli PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        target_compile_options(JiStatsMiniMaxCli PRIVATE /utf-8 /W4 /EHsc /FS)
    elseif(MINGW)
        target_sources(JiStatsMiniMaxCli PRIVATE platform/MinGwCompat.cpp)
    endif()

    install(TARGETS JiStatsMiniMaxCli)
endif()
