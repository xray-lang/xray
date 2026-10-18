
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
add_test(NAME test_xir_public_cli_scalar_methods
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_scalar_methods.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_scalar_methods PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;scalar")

add_executable(test_xir_scalar_methods_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_scalar_methods_vm.c)
target_include_directories(test_xir_scalar_methods_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_scalar_methods_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_scalar_methods_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_scalar_methods_vm PRIVATE -Wall -Wextra -Werror)
endif()

foreach(mode IN ITEMS measure faults)
    add_test(NAME test_xir_scalar_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    if(mode STREQUAL "measure")
        set(scalar_binding_mode binding-measure)
    else()
        set(scalar_binding_mode faults)
    endif()
    add_test(NAME test_xir_scalar_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${scalar_binding_mode})
    add_test(NAME test_xir_scalar_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/scalar_methods_resources.py
            $<TARGET_FILE:test_xir_scalar_methods_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_scalar_preparation_${mode} test_xir_scalar_binding_${mode} test_xir_scalar_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;scalar")
endforeach()

add_test(NAME test_xir_public_cli_integer_bits
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_integer_bits.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_integer_bits PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;integer")
add_executable(test_xir_integer_bits_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_integer_bits_vm.c)
target_include_directories(test_xir_integer_bits_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_integer_bits_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_integer_bits_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_integer_bits_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(bits_binding_mode binding-measure)
    else()
        set(bits_binding_mode faults)
    endif()
    add_test(NAME test_xir_integer_bits_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_integer_bits_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${bits_binding_mode})
    add_test(NAME test_xir_integer_bits_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/integer_bits_resources.py
            $<TARGET_FILE:test_xir_integer_bits_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_integer_bits_preparation_${mode} test_xir_integer_bits_binding_${mode} test_xir_integer_bits_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;integer")
endforeach()

add_test(NAME test_xir_public_cli_string_owned
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_string_owned.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_public_cli_string_owned PROPERTIES
    TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    LABELS "cli;xir;stdlib;native;execution;ownership;string")
add_executable(test_xir_string_owned_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_string_owned_vm.c)
target_include_directories(test_xir_string_owned_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_string_owned_vm PRIVATE xray_xir_stdlib_catalog xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_string_owned_vm PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_string_owned_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode IN ITEMS measure faults)
    if(mode STREQUAL "measure")
        set(string_binding_mode binding-measure)
    else()
        set(string_binding_mode faults)
    endif()
    add_test(NAME test_xir_string_owned_preparation_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_stdlib_preparation> ${PROJECT_SOURCE_DIR} ${mode})
    add_test(NAME test_xir_string_owned_binding_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_stdlib_resources> ${PROJECT_SOURCE_DIR} ${string_binding_mode})
    add_test(NAME test_xir_string_owned_runtime_${mode}
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/unit/xir/string_owned_resources.py
            $<TARGET_FILE:test_xir_string_owned_vm> ${PROJECT_SOURCE_DIR} ${mode})
    set_tests_properties(test_xir_string_owned_preparation_${mode} test_xir_string_owned_binding_${mode} test_xir_string_owned_runtime_${mode} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;ownership;stdlib;resources;string")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/integer_sqrt.cmake)
