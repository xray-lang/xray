cmake_minimum_required(VERSION 3.20)
project(xray_legacy_consumer_preparation LANGUAGES NONE)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Ninja is the only supported generator")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME legacy_consumer_preparation_negative_controls
    COMMAND "${Python3_EXECUTABLE}" -X utf8 -B "${CMAKE_CURRENT_SOURCE_DIR}/test_audit.py")
add_test(NAME legacy_consumer_preparation_inventory
    COMMAND "${Python3_EXECUTABLE}" -X utf8 -B "${CMAKE_CURRENT_SOURCE_DIR}/audit.py"
        snapshot --output "${CMAKE_CURRENT_BINARY_DIR}/inventory.json")
set_tests_properties(legacy_consumer_preparation_negative_controls legacy_consumer_preparation_inventory
    PROPERTIES LABELS "preparation-only" TIMEOUT 60)
