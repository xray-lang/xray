# The real CLI compiles, publishes and executes the same multi-module program.
add_test(NAME test_xir_public_cli_product
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_xir_public_cli_product.py
        --binary $<TARGET_FILE:xray> --root ${CMAKE_CURRENT_SOURCE_DIR}
        --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/xir_cli_product)
set_tests_properties(test_xir_public_cli_product PROPERTIES
    TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
