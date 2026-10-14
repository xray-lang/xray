add_test(NAME test_xir_public_cli_checked_families
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_checked_families.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_checked_families PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership")

add_test(NAME test_xir_public_cli_integer_methods
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_integer_methods.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_integer_methods PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;integer")

foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_integer_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_preparation_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
    if(ENABLE_ASAN AND mode STREQUAL "faults")
        set_tests_properties(test_xir_integer_preparation_${mode} PROPERTIES COST 300)
    endif()
endforeach()

add_executable(test_xir_integer_methods_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_integer_methods_vm.c)
target_include_directories(test_xir_integer_methods_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_integer_methods_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_integer_methods_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_integer_methods_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_integer_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_methods_resources.py
            $<TARGET_FILE:test_xir_integer_methods_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
endforeach()
