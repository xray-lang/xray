get_filename_component(XR_PRODUCER_TEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_compile_state
    "${CMAKE_CURRENT_LIST_DIR}/test_compile_state.c"
    "${XR_PRODUCER_TEST_ROOT}/src/base/xsource_cache.c"
    "${XR_PRODUCER_TEST_ROOT}/src/runtime/value/xtype_pool.c")
target_link_libraries(test_compile_state PRIVATE xray_compiler_parser)

target_include_directories(test_compile_state PRIVATE "${XR_PRODUCER_TEST_ROOT}/src" "${XR_PRODUCER_TEST_ROOT}/include")
target_compile_features(test_compile_state PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_compile_state PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_compile_state PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_compile_state PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_compile_state PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_compile_state COMMAND test_compile_state)
set_tests_properties(test_compile_state PROPERTIES LABELS "unit;compiler;ownership;budget")

add_executable(test_utf8_runtime_owner
    "${XR_PRODUCER_TEST_ROOT}/tests/unit/base/test_utf8_diagnostic.c"
    "${XR_PRODUCER_TEST_ROOT}/src/base/xutf8.c"
    "${XR_PRODUCER_TEST_ROOT}/src/base/xsimd.c")
target_include_directories(test_utf8_runtime_owner PRIVATE "${XR_PRODUCER_TEST_ROOT}/src" "${XR_PRODUCER_TEST_ROOT}/include")
target_compile_features(test_utf8_runtime_owner PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_utf8_runtime_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_utf8_runtime_owner PRIVATE /W3 /WX /utf-8)
endif()
add_test(NAME test_utf8_runtime_owner COMMAND test_utf8_runtime_owner)


if(WIN32)
    set(XR_PRODUCER_FD_SOURCE "${XR_PRODUCER_TEST_ROOT}/src/os/win/fd_win.c")
else()
    set(XR_PRODUCER_FD_SOURCE "${XR_PRODUCER_TEST_ROOT}/src/os/unix/fd_unix.c")
endif()
add_executable(test_diag_runtime_owner
    "${XR_PRODUCER_TEST_ROOT}/tests/unit/frontend/test_diag_fmt.c"
    "${XR_PRODUCER_FD_SOURCE}")
target_include_directories(test_diag_runtime_owner PRIVATE "${XR_PRODUCER_TEST_ROOT}/src" "${XR_PRODUCER_TEST_ROOT}/include")
target_compile_features(test_diag_runtime_owner PRIVATE c_std_11)
if(MSVC)
    target_compile_definitions(test_diag_runtime_owner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_diag_runtime_owner PRIVATE /W3 /WX /utf-8)
endif()
add_test(NAME test_diag_runtime_owner COMMAND test_diag_runtime_owner)
