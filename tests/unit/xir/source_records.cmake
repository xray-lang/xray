

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/record-cold")

add_test(NAME xir_record_source COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_records.py"
    --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
set_tests_properties(xir_record_source PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;records;ownership")
if(WIN32)
    add_test(NAME xir_record_native COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_source_records.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray> --native)
    set_tests_properties(xir_record_native PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;source;records;native;ownership")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/record-empty-stdlib")
add_test(NAME xir_record_sdk COMMAND test_hir_body_source "${CMAKE_CURRENT_BINARY_DIR}/record-empty-stdlib"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_records/readme.xr" "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_records/readme.out")
set_tests_properties(xir_record_sdk PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;records;sdk;ownership")
