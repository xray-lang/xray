# The real CLI compiles, publishes and executes the same multi-module program.
add_test(NAME test_xir_public_cli_product
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_public_cli_product.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --fixtures ${PROJECT_SOURCE_DIR}/tests/fixtures/xir_cli_product)
set_tests_properties(test_xir_public_cli_product PROPERTIES
    TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
if(WIN32)
    add_test(NAME test_xir_installed_product
        COMMAND ${XRAY_PYTHON} -B ${PROJECT_SOURCE_DIR}/tests/cli/run_xir_installed_product.py
            --root ${PROJECT_SOURCE_DIR} --build ${PROJECT_BINARY_DIR})
    set_tests_properties(test_xir_installed_product PROPERTIES
        TIMEOUT 300 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;xir;installation;native;stdlib")
endif()
