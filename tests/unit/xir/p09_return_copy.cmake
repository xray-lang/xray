set(P09_RETURN_COPY_C "${CMAKE_BINARY_DIR}/generated/xir_p09_return_copy.c")
add_executable(test_xir_p09_return_copy "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_return_copy.c")
target_link_libraries(test_xir_p09_return_copy PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${P09_RETURN_COPY_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_return_copy> 0 "${P09_RETURN_COPY_C}"
    DEPENDS test_xir_p09_return_copy
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/return_copy/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/return_copy/producer.xr" VERBATIM)
add_executable(test_xir_p09_return_copy_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_return_copy.c" "${P09_RETURN_COPY_C}")
target_link_libraries(test_xir_p09_return_copy_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_p09_return_copy_native PRIVATE XR_CAPACITY_NATIVE=1)
foreach(target test_xir_p09_return_copy test_xir_p09_return_copy_native)
    target_compile_definitions(${target} PRIVATE XR_CAPACITY_COMPONENT="return_copy"
        XR_CAPACITY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(return_copy_target test_xir_p09_return_copy)
    else()
        set(return_copy_target test_xir_p09_return_copy_native)
    endif()
    add_test(NAME test_xir_p09_return_copy_${mode} COMMAND ${return_copy_target} ${mode})
    set_tests_properties(test_xir_p09_return_copy_${mode} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;array;ownership;memory;cancel;execution;budget")
    add_test(NAME test_xir_p09_return_copy_normal_${mode} COMMAND ${return_copy_target} ${mode} --normal)
    set_tests_properties(test_xir_p09_return_copy_normal_${mode} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;array;ownership;execution")
    add_test(NAME test_xir_p09_return_copy_compiler_${mode} COMMAND ${return_copy_target} --compiler ${mode})
    set_tests_properties(test_xir_p09_return_copy_compiler_${mode} PROPERTIES TIMEOUT 600 COST 100
        LABELS "unit;xir;array;ownership;memory;execution;budget;compiler-fault")
endforeach()
