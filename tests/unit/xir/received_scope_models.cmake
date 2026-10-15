add_test(NAME retired_schema28_independent_vectors
    COMMAND "${XRAY_PYTHON}" -X utf8 -B "${CMAKE_CURRENT_LIST_DIR}/derive_retired_schema28_vectors.py")
set_tests_properties(retired_schema28_independent_vectors PROPERTIES LABELS "unit;xir;codec;independent-model" TIMEOUT 30)
