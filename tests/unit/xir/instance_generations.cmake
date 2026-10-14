add_executable(test_xir_instance_generations xir/test_xir_instance_generations.c)
target_link_libraries(test_xir_instance_generations PRIVATE xray_xir_vm xray_xir_cgen)
if(MSVC)
    target_compile_options(test_xir_instance_generations PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_instance_generations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_instance_generations COMMAND test_xir_instance_generations)
set_tests_properties(test_xir_instance_generations PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation" TIMEOUT 120)
add_test(NAME test_xir_instance_generations_runtime_fi
    COMMAND test_xir_instance_generations --runtime-fi)
set_tests_properties(test_xir_instance_generations_runtime_fi PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation;fault-injection" TIMEOUT 120)
add_test(NAME test_xir_instance_generations_runtime_axes
    COMMAND test_xir_instance_generations --runtime-axes)
set_tests_properties(test_xir_instance_generations_runtime_axes PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation;resources" TIMEOUT 120)
add_test(NAME test_xir_instance_generations_runtime_prepare
    COMMAND test_xir_instance_generations --runtime-prepare)
set_tests_properties(test_xir_instance_generations_runtime_prepare PROPERTIES
    LABELS "unit;xir;lifecycle;ownership;generation;resources" TIMEOUT 120)

foreach(generation_backend IN ITEMS vm native vm_native native_vm)
    set(generation_target test_xir_instance_generation_${generation_backend})
    add_executable(${generation_target} xir/test_xir_instance_generation_backends.c ${RN_NATIVE_C} "${RN_NATIVE_H}")
    target_include_directories(${generation_target} PRIVATE "${RN_NATIVE_DIR}")
    target_compile_definitions(${generation_target} PRIVATE
        XR_ROOT_NATIVE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_execution_native")
    if(generation_backend STREQUAL "native")
        target_compile_definitions(${generation_target} PRIVATE XR_GENERATION_OLD_VM=0 XR_GENERATION_NEW_VM=0)
        target_link_libraries(${generation_target} PRIVATE xray_xir_scalar)
    else()
        target_link_libraries(${generation_target} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
        if(generation_backend STREQUAL "vm")
            target_compile_definitions(${generation_target} PRIVATE XR_GENERATION_OLD_VM=1 XR_GENERATION_NEW_VM=1)
        elseif(generation_backend STREQUAL "vm_native")
            target_compile_definitions(${generation_target} PRIVATE XR_GENERATION_OLD_VM=1 XR_GENERATION_NEW_VM=0)
        else()
            target_compile_definitions(${generation_target} PRIVATE XR_GENERATION_OLD_VM=0 XR_GENERATION_NEW_VM=1)
        endif()
    endif()
    set_target_properties(${generation_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${generation_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${generation_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${generation_target} COMMAND ${generation_target})
    set_tests_properties(${generation_target} PROPERTIES
        LABELS "unit;xir;lifecycle;ownership;generation;source;backend" TIMEOUT 120)
endforeach()
