add_test(NAME test_xir_source_method_surface
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_method_surface.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
set_tests_properties(test_xir_source_method_surface PROPERTIES TIMEOUT 120 LABELS "unit;parser;xir;source;ownership")
if(WIN32)
    add_test(NAME test_xir_source_method_surface_native
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_method_surface.py"
            --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray> --native)
    set_tests_properties(test_xir_source_method_surface_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;parser;xir;source;ownership;native")
endif()
