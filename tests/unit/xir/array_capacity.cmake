# Genuine C is generated from the complete bounded Source pipeline.
set(CAPACITY_C "${CMAKE_BINARY_DIR}/generated/xir_array_capacity.c")
add_executable(test_xir_array_capacity "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_capacity.c")
target_link_libraries(test_xir_array_capacity PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${CAPACITY_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_capacity> 0 "${CAPACITY_C}"
    DEPENDS test_xir_array_capacity "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/transaction/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/transaction/producer.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/transaction/shadow.xr" VERBATIM)
add_executable(test_xir_array_capacity_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_capacity.c" "${CAPACITY_C}")
target_link_libraries(test_xir_array_capacity_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_capacity_native PRIVATE XR_CAPACITY_NATIVE=1)
foreach(target test_xir_array_capacity test_xir_array_capacity_native)
    target_compile_definitions(${target} PRIVATE XR_CAPACITY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_capacity_vm COMMAND test_xir_array_capacity 0)
add_test(NAME test_xir_array_capacity_native COMMAND test_xir_array_capacity_native 1)
add_test(NAME test_xir_array_capacity_mixed_root COMMAND test_xir_array_capacity_native 2)
add_test(NAME test_xir_array_capacity_mixed_import COMMAND test_xir_array_capacity_native 3)
set_tests_properties(test_xir_array_capacity_vm test_xir_array_capacity_native test_xir_array_capacity_mixed_root
    test_xir_array_capacity_mixed_import PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution")
add_test(NAME test_xir_array_capacity_source_rejections COMMAND test_xir_array_capacity --rejections)
set_tests_properties(test_xir_array_capacity_source_rejections PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;permission")
add_test(NAME test_xir_array_capacity_vm_census COMMAND test_xir_array_capacity --census 0)
add_test(NAME test_xir_array_capacity_vm_cancel COMMAND test_xir_array_capacity --cancel 0)
set_tests_properties(test_xir_array_capacity_vm_census test_xir_array_capacity_vm_cancel
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;memory;cancel;budget")

foreach(mode RANGE 1 3)
    if(mode EQUAL 1)
        set(capacity_runtime_mode native)
    elseif(mode EQUAL 2)
        set(capacity_runtime_mode mixed_root)
    else()
        set(capacity_runtime_mode mixed_import)
    endif()
    add_test(NAME test_xir_array_capacity_${capacity_runtime_mode}_census
        COMMAND test_xir_array_capacity_native --census ${mode})
    add_test(NAME test_xir_array_capacity_${capacity_runtime_mode}_cancel
        COMMAND test_xir_array_capacity_native --cancel ${mode})
    set_tests_properties(test_xir_array_capacity_${capacity_runtime_mode}_census
        test_xir_array_capacity_${capacity_runtime_mode}_cancel
        PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;memory;cancel;budget")
endforeach()
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(capacity_compiler_target test_xir_array_capacity)
    else()
        set(capacity_compiler_target test_xir_array_capacity_native)
    endif()
    if(WIN32)
        add_test(NAME test_xir_array_capacity_compiler_${mode}
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/xir/run_array_capacity_compiler.py"
            --executable $<TARGET_FILE:${capacity_compiler_target}> --mode ${mode} --workers 8
            --reports-root "${CMAKE_BINARY_DIR}/array-capacity-compiler/transaction_${mode}")
    else()
        add_test(NAME test_xir_array_capacity_compiler_${mode}
            COMMAND ${capacity_compiler_target} --compiler ${mode})
    endif()
    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        if(mode LESS 2)
            set(capacity_compiler_cost 94)
        else()
            set(capacity_compiler_cost 168)
        endif()
    else()
        set(capacity_compiler_cost 21)
    endif()
    set_tests_properties(test_xir_array_capacity_compiler_${mode}
        PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST ${capacity_compiler_cost}
        LABELS "unit;xir;array;ownership;memory;budget")
    if(WIN32)
        set_tests_properties(test_xir_array_capacity_compiler_${mode} PROPERTIES PROCESSORS 8)
    endif()
endforeach()
