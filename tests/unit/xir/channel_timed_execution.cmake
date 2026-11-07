

add_test(NAME test_xir_channel_timed_vectors COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_channel_timed76_vectors.py" --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}/tests/unit/xir")
set_tests_properties(test_xir_channel_timed_vectors PROPERTIES LABELS "unit;xir;channel;checked;metadata" TIMEOUT 30)

add_test(NAME test_xir_semantic76_retained_vectors COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_retained76_vectors.py" --source-root "${PROJECT_SOURCE_DIR}" --output-root "${PROJECT_SOURCE_DIR}/tests/unit/xir")
set_tests_properties(test_xir_semantic76_retained_vectors PROPERTIES LABELS "unit;xir;checked;metadata" TIMEOUT 30)

foreach(mode controls faults)

    set(timeout 60)
    set(consumer_labels "unit;xir;channel;ownership;runtime;resources")
    if(mode STREQUAL "faults")
        set(timeout 300)
        list(APPEND consumer_labels faults)
    endif()

endforeach()
