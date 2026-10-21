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

if(WIN32 AND XR_TARGET_ARCH MATCHES "^(x86_64|AMD64|amd64)$")
add_test(NAME test_cli_resource_profiles
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-basic --suite basic)
set_tests_properties(test_cli_resource_profiles PROPERTIES
    TIMEOUT 300 PROCESSORS 1 LABELS "cli;resources;usability")
endif()

# Two default executions and two finite-profile rejections, each run once.
# COST rounds the measured 94.328-second Windows Release gate up to 95 seconds.
# The 840-second sum of command timeouts leaves time for owned-tree cleanup.
if(XR_TARGET_ARCH MATCHES "^(x86_64|AMD64|amd64)$")
add_test(NAME test_cli_resource_profiles_large
    COMMAND ${XRAY_PYTHON} -X utf8 ${PROJECT_SOURCE_DIR}/tests/cli/run_resource_profiles.py
        --binary $<TARGET_FILE:xray> --root ${PROJECT_SOURCE_DIR}
        --output ${CMAKE_CURRENT_BINARY_DIR}/resource-profiles-large --suite large)
set_tests_properties(test_cli_resource_profiles_large PROPERTIES
    TIMEOUT 900 COST 95 PROCESSORS 1 RUN_SERIAL TRUE LABELS "cli;resources;large")
endif()
