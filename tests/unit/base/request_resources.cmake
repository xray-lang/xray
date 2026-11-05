# Shared focused registration; the isolated project never builds old consumers.
get_filename_component(XR_REQUEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(WIN32)
    set(XR_REQUEST_CLOCK "${XR_REQUEST_ROOT}/src/os/win/time_win.c")
else()
    set(XR_REQUEST_CLOCK "${XR_REQUEST_ROOT}/src/os/unix/time_unix.c")
endif()
add_library(compile_request_foundation_production OBJECT
    "${XR_REQUEST_ROOT}/src/base/xcompile_resources.c"
    "${XR_REQUEST_ROOT}/src/base/xcompile_state.c"
    "${XR_REQUEST_ROOT}/src/base/xcompile_request.c")
add_executable(test_compile_request_memory
    "${CMAKE_CURRENT_LIST_DIR}/test_compile_request_memory.c"
    "${XR_REQUEST_ROOT}/src/base/xcompile_request.c" "${XR_REQUEST_CLOCK}")
add_executable(test_compile_request_deadline "${CMAKE_CURRENT_LIST_DIR}/test_compile_request_deadline.c")
foreach(request_target compile_request_foundation_production test_compile_request_memory test_compile_request_deadline)
    target_include_directories(${request_target} PRIVATE "${XR_REQUEST_ROOT}/src")
    target_compile_features(${request_target} PRIVATE c_std_11)
    if(MSVC)
        if(XR_REQUEST_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN)
            set_property(TARGET ${request_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        else()
            set_property(TARGET ${request_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        target_compile_definitions(${request_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${request_target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${request_target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${request_target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME test_compile_request_memory COMMAND test_compile_request_memory)
add_test(NAME test_compile_request_deadline COMMAND test_compile_request_deadline)
set_tests_properties(test_compile_request_memory test_compile_request_deadline
    PROPERTIES LABELS "unit;compiler;ownership;request" TIMEOUT 30)
