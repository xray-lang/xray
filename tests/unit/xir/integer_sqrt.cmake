add_test(NAME test_xir_public_cli_integer_sqrt
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_integer_sqrt.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_integer_sqrt PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;integer")
add_executable(test_xir_integer_sqrt_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_integer_sqrt_vm.c)
target_include_directories(test_xir_integer_sqrt_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_integer_sqrt_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_integer_sqrt_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_integer_sqrt_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(sqrt_binding_mode binding-measure)
    else()
        set(sqrt_binding_mode faults)
    endif()
    add_test(NAME test_xir_integer_sqrt_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_sqrt_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_integer_sqrt_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_sqrt_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${sqrt_binding_mode})
    add_test(NAME test_xir_integer_sqrt_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_sqrt_resources.py
            $<TARGET_FILE:test_xir_integer_sqrt_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_sqrt_preparation_${mode} test_xir_integer_sqrt_binding_${mode} test_xir_integer_sqrt_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
endforeach()
