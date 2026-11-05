add_test(NAME test_xir_source_array_bindings
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_array_bindings.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
set_tests_properties(test_xir_source_array_bindings PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;array;ownership")
if(WIN32)
    add_test(NAME test_xir_source_array_bindings_native
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_array_bindings.py"
            --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray> --native)
    set_tests_properties(test_xir_source_array_bindings_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;source;array;native;ownership")
endif()
