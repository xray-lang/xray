set(native_clock_platform "${PROJECT_SOURCE_DIR}/src/os/unix/time_unix.c")
set(native_output_platform "${PROJECT_SOURCE_DIR}/src/os/native_output/native_output_unix.c")
set(native_filesystem_platform unix)
if(WIN32)
    set(native_clock_platform "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    set(native_output_platform "${PROJECT_SOURCE_DIR}/src/os/native_output/native_output_win.c")
    set(native_filesystem_platform win)
endif()
add_library(xray_native_runtime STATIC xnative_output.c xnative_locations.c xnative_status.c
    xnative_execution.c xnative_float.c xnative_module_state.c
    xnative_managed.c xnative_managed_root.c xnative_place.c
    xnative_backing_types.c xnative_string_backing.c xnative_string_backing_ops.c xnative_values.c
    xnative_array_backing.c xnative_array_ops.c xnative_array_slice.c xnative_map_backing.c xnative_set_backing.c xnative_value_plan.c xnative_value_image.c xnative_capture_backing.c
    xnative_class_array.c xnative_host_path.c xnative_host_file.c xnative_host_file_publish.c
    "${PROJECT_SOURCE_DIR}/src/os/${native_filesystem_platform}/file_mutation_${native_filesystem_platform}.c"
    "${PROJECT_SOURCE_DIR}/src/os/${native_filesystem_platform}/file_metadata_${native_filesystem_platform}.c"
    "${PROJECT_SOURCE_DIR}/src/os/${native_filesystem_platform}/file_links_${native_filesystem_platform}.c"
    "${PROJECT_SOURCE_DIR}/src/os/${native_filesystem_platform}/file_owned_${native_filesystem_platform}.c"
    "${native_output_platform}" "${native_clock_platform}")
add_dependencies(xray_native_runtime gen-native-type-defs gen-target-query-registry)
target_include_directories(xray_native_runtime PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(xray_native_runtime PUBLIC c_std_11)
set_target_properties(xray_native_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(xray_native_runtime PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(xray_native_runtime PRIVATE /clang:-ffp-contract=off)
    else()
        target_compile_options(xray_native_runtime PRIVATE /fp:precise)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(xray_native_runtime PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(xray_native_runtime PRIVATE -Wall -Wextra -Werror -pedantic -ffp-contract=off)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(xray_native_runtime PRIVATE Threads::Threads)
    target_compile_definitions(xray_native_runtime PRIVATE _POSIX_C_SOURCE=200809L)
endif()

# Direct generated-C clients consume the same canonical archive closure.
set(XRAY_NATIVE_RUNTIME_LINK_INPUTS $<TARGET_FILE:xray_native_runtime>)
if(WIN32)
    list(APPEND XRAY_NATIVE_RUNTIME_LINK_INPUTS advapi32.lib userenv.lib)
endif()
set(XRAY_NATIVE_RUNTIME_LINK_INPUTS ${XRAY_NATIVE_RUNTIME_LINK_INPUTS} PARENT_SCOPE)
