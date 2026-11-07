

add_test(NAME test_xir_union86_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/derive_union86_vectors.py)
add_test(NAME test_xir_union86_retired_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/derive_union86_retired.py)
set_tests_properties(test_xir_union86_vectors test_xir_union86_retired_vectors PROPERTIES TIMEOUT 120 LABELS "unit;xir;checked;independent;ownership")
if(WIN32)
    add_test(NAME test_xir_union86_sdk_identity COMMAND test_xir_runtime_sdk
        ${CMAKE_BINARY_DIR}/xir-runtime-sdk ${CMAKE_BINARY_DIR}/xir-runtime-sdk/sdk_manifest.json status)
    set_tests_properties(test_xir_union86_sdk_identity PROPERTIES TIMEOUT 30 LABELS "unit;xir;sdk;abi;independent")
endif()
