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

# A supplied fresh public executable enables actual consumers. Missing binary
# leaves preparation tests separate; it never supplies a fake product result.
set(XRAY_LEGACY_PRODUCT_BINARY "" CACHE FILEPATH "Fresh public CLI for preserved consumers")
set(XRAY_LEGACY_PRODUCT_COMPILER "" CACHE FILEPATH "Explicit native C provider")
if(XRAY_LEGACY_PRODUCT_BINARY)
    if(NOT EXISTS "${XRAY_LEGACY_PRODUCT_BINARY}")
        message(FATAL_ERROR "The requested real public executable does not exist")
    endif()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/product_driver_cases.json" consumer_manifest)
    string(JSON consumer_count LENGTH "${consumer_manifest}" cases)
    math(EXPR consumer_last "${consumer_count}-1")
    foreach(index RANGE 0 ${consumer_last})
        string(JSON identity GET "${consumer_manifest}" cases ${index} id)
        set(provider_args)
        if(XRAY_LEGACY_PRODUCT_COMPILER)
            list(APPEND provider_args --compiler "${XRAY_LEGACY_PRODUCT_COMPILER}")
        endif()
        add_test(NAME "legacy_product_${identity}"
            COMMAND "${Python3_EXECUTABLE}" -X utf8 -B
                "${CMAKE_CURRENT_SOURCE_DIR}/run_product_driver.py"
                --binary "${XRAY_LEGACY_PRODUCT_BINARY}" ${provider_args}
                --output "${CMAKE_CURRENT_BINARY_DIR}/product-evidence/${identity}"
                --case "${identity}")
        set_tests_properties("legacy_product_${identity}" PROPERTIES
            TIMEOUT 120 LABELS "actual-product-consumer;cli;vm;native" PROCESSORS 1 RUN_SERIAL TRUE)
    endforeach()
endif()
