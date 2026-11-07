# Both emitted programs retain the same checked proof and typed default owner.
set(XIR_ASSERT_PANICS_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-panics-source)
set(XIR_ASSERT_PANICS_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_panics.c)
set(XIR_ASSERT_PANICS_MATRIX_C ${XIR_ASSERT_PANICS_C}.matrix.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_PANICS_DIR})

add_test(NAME test_xir_assert_panics_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_panics_vector.py)
add_test(NAME test_xir_assert_panics_current_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_construction72_vectors.py
        --source-root ${CMAKE_SOURCE_DIR} --output-root ${CMAKE_SOURCE_DIR})
add_test(NAME test_xir_assert_panics_current_core_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_panics_current_vector.py)
set_tests_properties(test_xir_assert_panics_current_core_vector PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion")
add_test(NAME test_xir_binder_wire_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_binder56_migration.py)
set_tests_properties(test_xir_assert_panics_wire_vector test_xir_assert_panics_current_wire_vector test_xir_binder_wire_vectors PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion")

# Independent current complete family fields; no compiler executable is an oracle.
add_test(NAME test_xir_construction_family_wire_vector COMMAND ${XRAY_PYTHON}
    "${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_construction_family72.py"
    --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_construction_family_wire_vector PROPERTIES TIMEOUT 30 LABELS "unit;xir;abi;independent-golden")

# Current common frames keep all complete historical construction vectors.
add_test(NAME test_xir_cell_checked_identity_vectors COMMAND ${XRAY_PYTHON}
    "${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_cell73_checked_vectors.py"
    --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}")
set_tests_properties(test_xir_cell_checked_identity_vectors PROPERTIES
    TIMEOUT 30 LABELS "unit;xir;abi;independent-golden")
