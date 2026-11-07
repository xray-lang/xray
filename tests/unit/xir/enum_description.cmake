

include("${CMAKE_CURRENT_LIST_DIR}/generated_built.cmake")
add_test(NAME test_xir_nominal_authority COMMAND test_xir_checked --nominal-authority)
set_tests_properties(test_xir_nominal_authority PROPERTIES TIMEOUT 30
    LABELS "unit;xir;metadata;ownership;authority")

add_test(NAME test_xir_combined_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/derive_combined77_vectors.py)
set_tests_properties(test_xir_combined_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;identity;independent-golden")
