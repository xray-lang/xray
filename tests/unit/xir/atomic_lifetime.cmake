get_filename_component(XIR_MIXED_ATOMIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_xir_mixed_atomic_producer "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_atomic_producer.c")
target_link_libraries(test_xir_mixed_atomic_producer PRIVATE xray_xir_source xray_xir_cgen)
set(XIR_MIXED_ATOMIC_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_atomic.c")
add_custom_command(OUTPUT "${XIR_MIXED_ATOMIC_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_atomic_producer> "${XIR_MIXED_ATOMIC_C}"
    DEPENDS test_xir_mixed_atomic_producer
        "${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_runtime/root.xr"
        "${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_runtime/worker.xr"
        "${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_permissions/state.xr"
    VERBATIM)
add_executable(test_xir_mixed_atomic_lifetime "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_atomic_lifetime.c" "${XIR_MIXED_ATOMIC_C}")
add_executable(test_xir_mixed_atomic_source_compiler "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_atomic_source_compiler.c" "${XIR_MIXED_ATOMIC_C}")
foreach(target IN ITEMS test_xir_mixed_atomic_lifetime test_xir_mixed_atomic_source_compiler)
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm xray_xir_scalar)
endforeach()
foreach(target IN ITEMS test_xir_mixed_atomic_producer test_xir_mixed_atomic_lifetime test_xir_mixed_atomic_source_compiler)
    target_compile_definitions(${target} PRIVATE
        XR_P20_ATOMIC_RUNTIME_FIXTURES="${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_runtime"
        XR_P20_ATOMIC_AUTHORITY="${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures"
        XR_SOURCE_STDLIB="${XIR_MIXED_ATOMIC_ROOT}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_atomic_lifetime_${backend} COMMAND test_xir_mixed_atomic_lifetime ${backend})
    set_tests_properties(test_xir_mixed_atomic_lifetime_${backend} PROPERTIES TIMEOUT 120 PROCESSORS 1 COST 2
        LABELS "unit;xir;mixed;atomic;permissions;lifecycle;ownership;generated-c")
    add_test(NAME test_xir_mixed_atomic_source_compiler_${backend} COMMAND test_xir_mixed_atomic_source_compiler ${backend})
    set_tests_properties(test_xir_mixed_atomic_source_compiler_${backend} PROPERTIES TIMEOUT 120 PROCESSORS 1 COST 2
        LABELS "unit;xir;mixed;atomic;source;compiler;ownership;generated-c")
endforeach()
