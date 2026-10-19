add_executable(test_xir_mixed_atomic_compiler_resources "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_atomic_compiler_resources.c" "${XIR_MIXED_ATOMIC_C}")
target_link_libraries(test_xir_mixed_atomic_compiler_resources PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_atomic_compiler_resources PRIVATE
    XR_P20_ATOMIC_RUNTIME_FIXTURES="${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_runtime"
    XR_P20_ATOMIC_AUTHORITY="${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures"
    XR_SOURCE_STDLIB="${XIR_MIXED_ATOMIC_ROOT}/stdlib")
set_target_properties(test_xir_mixed_atomic_compiler_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_atomic_compiler_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_atomic_compiler_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_atomic_compiler_resources_${backend} COMMAND test_xir_mixed_atomic_compiler_resources ${backend} axes)
    set_tests_properties(test_xir_mixed_atomic_compiler_resources_${backend} PROPERTIES TIMEOUT 120 PROCESSORS 1 COST 4
        LABELS "unit;xir;mixed;atomic;source;compiler;resources;ownership;generated-c")
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/atomic_compiler_fi_cases.cmake")
