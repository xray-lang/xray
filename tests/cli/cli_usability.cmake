# First-use regressions run the real build-tree CLI: default stdlib discovery
# for an uninstalled (or package-configured) build and UTF-8 BOM sources.
set(_xray_cli_usability_source_tree "")
if(XRAY_SOURCE_TREE_STDLIB)
    set(_xray_cli_usability_source_tree --source-tree-stdlib "${PROJECT_SOURCE_DIR}/stdlib")
endif()
add_test(NAME test_cli_usability
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_cli_usability.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/cli-usability
        ${_xray_cli_usability_source_tree})
set_tests_properties(test_cli_usability PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "cli;usability;stdlib")
