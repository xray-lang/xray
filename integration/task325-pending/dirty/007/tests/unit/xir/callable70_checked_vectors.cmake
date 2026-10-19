# Verify independent complete current packets and exact historical models.
if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
add_test(NAME test_xir_callable70_checked_vectors
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/derive_callable70_checked_vectors.py" --verify)
set_tests_properties(test_xir_callable70_checked_vectors PROPERTIES LABELS "unit;xir;metadata;ownership;abi" TIMEOUT 60)
