if(NOT WIN32)
    return()
endif()
foreach(mode injected production)
    add_executable(test_windows_path_${mode}
        "${CMAKE_CURRENT_LIST_DIR}/windows_path_owner/test_windows_path.c"
        "${PROJECT_SOURCE_DIR}/src/base/xio_policy.c"
        "${PROJECT_SOURCE_DIR}/src/base/xfileio.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/fs_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/dir_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/file_read_win.c")
    if(mode STREQUAL "production")
        target_compile_definitions(test_windows_path_${mode} PRIVATE WINDOWS_PATH_PRODUCTION)
    endif()
    target_include_directories(test_windows_path_${mode} PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(test_windows_path_${mode} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(test_windows_path_${mode} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(test_windows_path_${mode} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_windows_path_${mode} PRIVATE /experimental:c11atomics)
    endif()
    add_test(NAME windows_path_${mode} COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/windows_path_owner/test_windows_path.py"
        $<TARGET_FILE:test_windows_path_${mode}>)
    set_tests_properties(windows_path_${mode} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 3
        LABELS "unit;windows;ownership;budget;filesystem")
endforeach()
