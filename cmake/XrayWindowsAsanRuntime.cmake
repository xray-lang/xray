# Windows loads Clang's dynamic ASan runtime before an instrumented process can
# enter main(). The DLL lives under the selected compiler's resource directory,
# not normally beside the executable or on PATH. Keep discovery tied to that
# compiler so CTest never picks a runtime from an unrelated LLVM installation.

function(xray_discover_windows_asan_runtime out_var)
    if(NOT CMAKE_HOST_WIN32 OR NOT ENABLE_ASAN)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(_xray_asan_dll "clang_rt.asan_dynamic-x86_64.dll")
    get_filename_component(_xray_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    set(_xray_asan_candidates "${_xray_compiler_dir}")

    foreach(_xray_print_option IN ITEMS --print-runtime-dir --print-resource-dir)
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" "${_xray_print_option}"
            RESULT_VARIABLE _xray_print_result
            OUTPUT_VARIABLE _xray_print_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
            TIMEOUT 30
        )
        if(_xray_print_result EQUAL 0 AND NOT _xray_print_output STREQUAL "")
            list(APPEND _xray_asan_candidates
                "${_xray_print_output}"
                "${_xray_print_output}/lib/windows"
            )
        endif()
    endforeach()

    list(REMOVE_DUPLICATES _xray_asan_candidates)
    foreach(_xray_asan_candidate IN LISTS _xray_asan_candidates)
        cmake_path(NORMAL_PATH _xray_asan_candidate
            OUTPUT_VARIABLE _xray_asan_candidate_normal)
        if(EXISTS "${_xray_asan_candidate_normal}/${_xray_asan_dll}")
            set(${out_var} "${_xray_asan_candidate_normal}" PARENT_SCOPE)
            message(STATUS
                "Windows ASan runtime: ${_xray_asan_candidate_normal}")
            return()
        endif()
    endforeach()

    string(JOIN "\n  " _xray_asan_searched ${_xray_asan_candidates})
    message(FATAL_ERROR
        "${_xray_asan_dll} was not found for ${CMAKE_C_COMPILER}.\n"
        "Searched compiler-owned directories:\n  ${_xray_asan_searched}")
endfunction()


function(xray_stage_windows_asan_runtime)
    if(NOT CMAKE_HOST_WIN32 OR NOT ENABLE_ASAN)
        return()
    endif()
    if(NOT XRAY_WINDOWS_ASAN_RUNTIME_DIR)
        message(FATAL_ERROR
            "Windows ASan outputs have no compiler-owned runtime directory")
    endif()

    set(_xray_asan_dll "clang_rt.asan_dynamic-x86_64.dll")
    set(_xray_asan_source
        "${XRAY_WINDOWS_ASAN_RUNTIME_DIR}/${_xray_asan_dll}")
    foreach(_xray_output_dir IN LISTS ARGN)
        file(MAKE_DIRECTORY "${_xray_output_dir}")
        # configure_file tracks the selected compiler artifact and refreshes a
        # stale build-tree copy on reconfigure. No install rule consumes it.
        configure_file(
            "${_xray_asan_source}"
            "${_xray_output_dir}/${_xray_asan_dll}"
            COPYONLY
        )
    endforeach()
endfunction()


function(xray_prepend_windows_asan_runtime_to_directory_tests)
    if(NOT CMAKE_HOST_WIN32 OR NOT ENABLE_ASAN)
        return()
    endif()
    if(NOT XRAY_WINDOWS_ASAN_RUNTIME_DIR)
        message(FATAL_ERROR
            "Windows ASan tests have no compiler-owned runtime directory")
    endif()

    get_property(_xray_directory_tests DIRECTORY PROPERTY TESTS)
    foreach(_xray_test IN LISTS _xray_directory_tests)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.22")
            # Append instead of replacing: individual tests may already carry
            # sanitizer or provider environment modifications of their own.
            set_property(TEST "${_xray_test}" APPEND PROPERTY
                ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${XRAY_WINDOWS_ASAN_RUNTIME_DIR}")
        else()
            # ENVIRONMENT_MODIFICATION arrived in CMake 3.22. Preserve the
            # project's 3.21 floor with an equivalent configure-time PATH.
            set(_xray_test_path
                "${XRAY_WINDOWS_ASAN_RUNTIME_DIR};$ENV{PATH}")
            string(REPLACE ";" "\\;" _xray_test_path "${_xray_test_path}")
            set_property(TEST "${_xray_test}" APPEND PROPERTY ENVIRONMENT
                "PATH=${_xray_test_path}")
        endif()
    endforeach()
endfunction()
