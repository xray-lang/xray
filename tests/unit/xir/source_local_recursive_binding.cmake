add_test(NAME test_xir_source_local_recursive_binding
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_local_recursive_binding.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
set_tests_properties(test_xir_source_local_recursive_binding PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;source;callable;ownership")
if(WIN32)
    add_test(NAME test_xir_source_local_recursive_binding_native
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_local_recursive_binding.py"
            --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray> --native)
    set_tests_properties(test_xir_source_local_recursive_binding_native PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;source;callable;ownership;native")
endif()
