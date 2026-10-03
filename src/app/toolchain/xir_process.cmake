# The prepared process owner uses the same production objects in full and standalone builds.
if(TARGET xray_toolchain_process)
    return()
endif()
if(NOT WIN32)
    message(FATAL_ERROR "The prepared process production library requires Windows")
endif()
get_filename_component(XIR_PROCESS_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT TARGET xray_compile_resources)
    add_subdirectory("${XIR_PROCESS_SOURCE_ROOT}/base" "${CMAKE_CURRENT_BINARY_DIR}/toolchain-process-base")
endif()
add_library(xray_toolchain_process STATIC
    "${CMAKE_CURRENT_LIST_DIR}/xtc_process.c"
    "${XIR_PROCESS_SOURCE_ROOT}/os/win/proc_win.c"
    "${XIR_PROCESS_SOURCE_ROOT}/os/win/proc_self_win.c"
    "${XIR_PROCESS_SOURCE_ROOT}/os/win/pipe_win.c"
    "${XIR_PROCESS_SOURCE_ROOT}/os/win/time_win.c"
    "${XIR_PROCESS_SOURCE_ROOT}/base/xutf8.c"
    "${XIR_PROCESS_SOURCE_ROOT}/base/xsha256.c")
target_link_libraries(xray_toolchain_process PUBLIC xray_compile_resources)
target_include_directories(xray_toolchain_process PUBLIC "${XIR_PROCESS_SOURCE_ROOT}"
    PRIVATE "${XIR_PROCESS_SOURCE_ROOT}/../include")
target_compile_features(xray_toolchain_process PUBLIC c_std_11)
set_target_properties(xray_toolchain_process PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(xray_toolchain_process PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(xray_toolchain_process PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(xray_toolchain_process PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(xray_toolchain_process PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
