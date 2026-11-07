

add_test(NAME test_xir_semantic65_role_oracle COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_semantic65_packets.py)
set_tests_properties(test_xir_semantic65_role_oracle PROPERTIES LABELS "unit;xir;checked;wire" TIMEOUT 60)
