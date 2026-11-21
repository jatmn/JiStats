if(NOT DEFINED JISTATS_UPX_EXECUTABLE OR JISTATS_UPX_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "JISTATS_UPX_EXECUTABLE must be provided to RunUpx.cmake")
endif()

if(NOT DEFINED JISTATS_UPX_TARGET OR JISTATS_UPX_TARGET STREQUAL "")
    message(FATAL_ERROR "JISTATS_UPX_TARGET must be provided to RunUpx.cmake")
endif()

if(NOT DEFINED JISTATS_UPX_OUTPUT OR JISTATS_UPX_OUTPUT STREQUAL "")
    set(JISTATS_UPX_OUTPUT "${JISTATS_UPX_TARGET}")
endif()

if(NOT DEFINED JISTATS_UPX_TARGET_NAME OR JISTATS_UPX_TARGET_NAME STREQUAL "")
    set(JISTATS_UPX_TARGET_NAME "${JISTATS_UPX_TARGET}")
endif()

if(NOT DEFINED JISTATS_UPX_CONFIG)
    set(JISTATS_UPX_CONFIG "")
endif()

if(JISTATS_UPX_CONFIG STREQUAL "Release")
    execute_process(
        COMMAND "${JISTATS_UPX_EXECUTABLE}" -d "${JISTATS_UPX_TARGET}"
        RESULT_VARIABLE JISTATS_UPX_UNPACK_RESULT
        OUTPUT_VARIABLE JISTATS_UPX_UNPACK_STDOUT
        ERROR_VARIABLE JISTATS_UPX_UNPACK_STDERR
    )

    get_filename_component(JISTATS_UPX_OUTPUT_DIR "${JISTATS_UPX_OUTPUT}" DIRECTORY)
    if(NOT JISTATS_UPX_OUTPUT_DIR STREQUAL "")
        file(MAKE_DIRECTORY "${JISTATS_UPX_OUTPUT_DIR}")
    endif()

    if(EXISTS "${JISTATS_UPX_OUTPUT}")
        file(REMOVE "${JISTATS_UPX_OUTPUT}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${JISTATS_UPX_TARGET}" "${JISTATS_UPX_OUTPUT}"
        RESULT_VARIABLE JISTATS_UPX_COPY_RESULT
        OUTPUT_VARIABLE JISTATS_UPX_COPY_STDOUT
        ERROR_VARIABLE JISTATS_UPX_COPY_STDERR
    )

    if(NOT JISTATS_UPX_COPY_RESULT EQUAL 0)
        string(CONCAT JISTATS_UPX_COPY_WARNING
            "Skipping UPX variant refresh for " "${JISTATS_UPX_TARGET_NAME}" ".\n"
            "source: " "${JISTATS_UPX_TARGET}" "\n"
            "dest: " "${JISTATS_UPX_OUTPUT}" "\n"
            "stdout:\n" "${JISTATS_UPX_COPY_STDOUT}" "\n"
            "stderr:\n" "${JISTATS_UPX_COPY_STDERR}" "\n"
            "The normal Release binary was built successfully; the side-by-side -upx variant is likely locked by a running process."
        )
        message(WARNING "${JISTATS_UPX_COPY_WARNING}")
        return()
    endif()

    execute_process(
        COMMAND "${JISTATS_UPX_EXECUTABLE}" --best --lzma "${JISTATS_UPX_OUTPUT}"
        RESULT_VARIABLE JISTATS_UPX_RESULT
        OUTPUT_VARIABLE JISTATS_UPX_STDOUT
        ERROR_VARIABLE JISTATS_UPX_STDERR
    )

    if(NOT JISTATS_UPX_RESULT EQUAL 0)
        string(CONCAT JISTATS_UPX_FAILURE
            "UPX packing failed for " "${JISTATS_UPX_TARGET_NAME}" " (" "${JISTATS_UPX_OUTPUT}" ").\n"
            "stdout:\n" "${JISTATS_UPX_STDOUT}" "\n"
            "stderr:\n" "${JISTATS_UPX_STDERR}"
        )
        message(FATAL_ERROR "${JISTATS_UPX_FAILURE}")
    endif()

    if(NOT JISTATS_UPX_STDOUT STREQUAL "")
        string(STRIP "${JISTATS_UPX_STDOUT}" JISTATS_UPX_STDOUT)
        message(STATUS "UPX packed ${JISTATS_UPX_TARGET_NAME} variant (${JISTATS_UPX_OUTPUT}): ${JISTATS_UPX_STDOUT}")
    endif()
endif()
