add_executable(test_xir_mixed_deep_producer xir/test_xir_mixed_deep_producer.c)
target_link_libraries(test_xir_mixed_deep_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_deep_producer PRIVATE
    XR_DEEP_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_worker_deep" XR_DEEP_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
set(deep_source "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_worker_deep.c")
add_custom_command(OUTPUT "${deep_source}"
    COMMAND $<TARGET_FILE:test_xir_mixed_deep_producer> "${deep_source}"
    DEPENDS test_xir_mixed_deep_producer xir/fixtures/p20_worker_deep/root.xr xir/fixtures/p20_worker_deep/callee.xr VERBATIM)
add_executable(test_xir_mixed_deep_recursion xir/test_xir_mixed_deep_recursion.c "${deep_source}")
target_link_libraries(test_xir_mixed_deep_recursion PRIVATE xray_xir_vm xray_xir_scalar)
foreach(deep_target IN ITEMS test_xir_mixed_deep_producer test_xir_mixed_deep_recursion)
    set_target_properties(${deep_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${deep_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${deep_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(deep_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_deep_${deep_backend} COMMAND test_xir_mixed_deep_recursion ${deep_backend})
    set_tests_properties(test_xir_mixed_deep_${deep_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;root;worker;generated-c;lifecycle;recursion;stack")
endforeach()
