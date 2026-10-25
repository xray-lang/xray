add_executable(test_xir_p09_string_mutation "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_mutation.c")
target_link_libraries(test_xir_p09_string_mutation PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
foreach(target test_xir_p09_string_mutation)
    target_compile_definitions(${target} PRIVATE XR_CAPACITY_COMPONENT="string_append"
        XR_CAPACITY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode RANGE 0 0)
    set(string_mutation_target test_xir_p09_string_mutation)
    add_test(NAME test_xir_p09_string_mutation_${mode} COMMAND ${string_mutation_target} ${mode})
    set_tests_properties(test_xir_p09_string_mutation_${mode} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;string;ownership;memory;cancel;execution;budget")
    add_test(NAME test_xir_p09_string_mutation_normal_${mode} COMMAND ${string_mutation_target} ${mode} --normal)
    set_tests_properties(test_xir_p09_string_mutation_normal_${mode} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;string;ownership;execution")
    add_test(NAME test_xir_p09_string_mutation_compiler_${mode} COMMAND ${string_mutation_target} --compiler ${mode})
    set_tests_properties(test_xir_p09_string_mutation_compiler_${mode} PROPERTIES TIMEOUT 600 COST 100
        LABELS "unit;xir;string;ownership;memory;execution;budget;compiler-fault")
endforeach()
