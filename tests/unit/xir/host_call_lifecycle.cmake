set(XIR_HOST_CALL_TEST_DIR ${CMAKE_CURRENT_LIST_DIR})
add_xray_bootstrap_executable(test_xir_host_call_source ${XIR_HOST_CALL_TEST_DIR}/test_xir_host_call_source.c)
target_link_libraries(test_xir_host_call_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_runtime_host)
target_compile_definitions(test_xir_host_call_source PRIVATE XR_HOST_CALL_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_host_call")
set(XIR_HOST_CALL_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_host_call.c)
add_custom_command(OUTPUT ${XIR_HOST_CALL_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_host_call_source> ${XIR_HOST_CALL_GENERATED}
    DEPENDS test_xir_host_call_source ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_host_call/root.xr
    VERBATIM)
add_executable(test_xir_host_call_native ${XIR_HOST_CALL_TEST_DIR}/test_xir_host_call_native.c ${XIR_HOST_CALL_GENERATED})
target_link_libraries(test_xir_host_call_native PRIVATE xray_xir_runtime_host)
add_executable(test_xir_host_call_linked ${XIR_HOST_CALL_TEST_DIR}/test_xir_host_call_linked.c ${XIR_HOST_CALL_GENERATED})
target_link_libraries(test_xir_host_call_linked PRIVATE xray_xir_runtime_host)
foreach(target test_xir_host_call_source test_xir_host_call_native test_xir_host_call_linked)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit;xir;execution;ownership" TIMEOUT 180)
endforeach()
add_test(NAME test_xir_host_call_fatal COMMAND ${XRAY_PYTHON} ${XIR_HOST_CALL_TEST_DIR}/host_call_fatal.py
    $<TARGET_FILE:test_xir_host_call_source> $<TARGET_FILE:test_xir_host_call_native>)
set_tests_properties(test_xir_host_call_fatal PROPERTIES LABELS "unit;xir;execution;ownership" TIMEOUT 180)
