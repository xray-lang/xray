# Retiring consumers retain exact source inputs and independent result goldens.
set(product_consumer_source "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_consumer.c")
set(product_consumer_fixture_root "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/fixtures")
get_filename_component(product_consumer_fixture_root "${product_consumer_fixture_root}" REALPATH)
set(product_consumer_fault_runner "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_faults.py")
set(product_consumer_main_template "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_main.c.in")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_cases.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_graph_cases.cmake")

foreach(case IN ITEMS bare_nullable missing_name)
    set(test "test_source_product_rejections_${case}")

endforeach()
add_test(NAME source_product_consumer_inventory
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_inventory.py" check)
set_tests_properties(source_product_consumer_inventory PROPERTIES TIMEOUT 30
    LABELS "unit;xir;source-product;program-consumer;inventory")
add_executable(test_source_product_integer_constructors
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_integer_constructors.c")
set_target_properties(test_source_product_integer_constructors PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_integer_constructors PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_integer_constructors PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_integer_constructors PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_product_integer_source_constructors
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_numeric_sources.py"
        --binary $<TARGET_FILE:test_source_product_integer_constructors>
        --scratch "${CMAKE_BINARY_DIR}/consumer-integer-constructor-evidence")
set_tests_properties(source_product_integer_source_constructors PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;inventory;integer")
add_executable(test_source_product_nominal_constructors
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_nominal_constructors.c")
target_link_libraries(test_source_product_nominal_constructors PRIVATE xray_xir_source_product)
set_target_properties(test_source_product_nominal_constructors PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_nominal_constructors PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_nominal_constructors PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_nominal_constructors PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME source_product_nominal_source_constructors
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_nominal_sources.py"
        --binary $<TARGET_FILE:test_source_product_nominal_constructors>
        --scratch "${CMAKE_BINARY_DIR}/consumer-nominal-constructor-evidence")
set_tests_properties(source_product_nominal_source_constructors PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;inventory;nominal-graph")
