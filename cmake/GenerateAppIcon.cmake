# Builds the Windows app icon on non-Windows hosts with ImageMagick.
# Matches the PowerShell generator in src/CMakeLists.txt: crop to pixels with
# alpha above 16, center on a square, then render each size with a size/12 inset.

foreach(required_var JISTATS_IMAGEMAGICK_EXECUTABLE JISTATS_ICON_INPUT JISTATS_ICON_OUTPUT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be provided to GenerateAppIcon.cmake")
    endif()
endforeach()

execute_process(
    COMMAND "${JISTATS_IMAGEMAGICK_EXECUTABLE}" "${JISTATS_ICON_INPUT}"
        -alpha extract -threshold 6.28% -format "%@" info:
    RESULT_VARIABLE bounds_result
    OUTPUT_VARIABLE bounds
    ERROR_VARIABLE bounds_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT bounds_result EQUAL 0 OR NOT bounds MATCHES "^([0-9]+)x([0-9]+)\\+([0-9]+)\\+([0-9]+)$")
    message(FATAL_ERROR "Could not measure ${JISTATS_ICON_INPUT}: ${bounds} ${bounds_error}")
endif()
set(content_width ${CMAKE_MATCH_1})
set(content_height ${CMAKE_MATCH_2})
if(content_width GREATER content_height)
    set(square_size ${content_width})
else()
    set(square_size ${content_height})
endif()

set(icon_args "${JISTATS_ICON_INPUT}" -crop "${bounds}" +repage
    -background none -gravity center -extent "${square_size}x${square_size}")
foreach(size 16 24 32 48 64 128 256)
    math(EXPR inset "${size} / 12")
    if(inset LESS 1)
        set(inset 1)
    endif()
    math(EXPR target "${size} - (${inset} * 2)")
    list(APPEND icon_args
        "(" -clone 0 -resize "${target}x${target}" -extent "${size}x${size}" ")")
endforeach()
list(APPEND icon_args -delete 0 "${JISTATS_ICON_OUTPUT}")

get_filename_component(output_dir "${JISTATS_ICON_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
execute_process(
    COMMAND "${JISTATS_IMAGEMAGICK_EXECUTABLE}" ${icon_args}
    RESULT_VARIABLE icon_result
    ERROR_VARIABLE icon_error
)
if(NOT icon_result EQUAL 0)
    message(FATAL_ERROR "ImageMagick failed to write ${JISTATS_ICON_OUTPUT}: ${icon_error}")
endif()
