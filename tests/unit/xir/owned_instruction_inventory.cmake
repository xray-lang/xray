add_executable(test_xir_owned_instruction_inventory
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_owned_instruction_inventory.c)
target_include_directories(test_xir_owned_instruction_inventory PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME test_xir_owned_instruction_inventory COMMAND test_xir_owned_instruction_inventory)
set_tests_properties(test_xir_owned_instruction_inventory PROPERTIES
    LABELS "unit;xir;ownership;budget" TIMEOUT 120 PROCESSORS 1)
