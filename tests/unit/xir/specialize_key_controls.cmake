# Long vectors and literal call order provide independent specialization keys.
add_test(NAME test_xir_specialize_key_controls
    COMMAND test_xir_source_generic_callable_context --spec-key-controls)
set_tests_properties(test_xir_specialize_key_controls PROPERTIES
    LABELS "unit;xir;specialization;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

# Component tests consume the actual private implementation and an independent oracle.
add_executable(test_xir_specialize_index ${CMAKE_CURRENT_LIST_DIR}/test_xir_specialize_index.c)
target_link_libraries(test_xir_specialize_index PRIVATE xray_xir_admission)
if(MSVC)
    target_compile_options(test_xir_specialize_index PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_specialize_index PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_specialize_index COMMAND test_xir_specialize_index)
set_tests_properties(test_xir_specialize_index PROPERTIES
    LABELS "unit;xir;specialization;ownership;budget" TIMEOUT 300)

add_executable(test_xir_specialize_scale ${CMAKE_CURRENT_LIST_DIR}/test_xir_specialize_scale.c)
target_link_libraries(test_xir_specialize_scale PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_specialize_scale PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_specialize_scale PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_specialize_scale COMMAND test_xir_specialize_scale)
set_tests_properties(test_xir_specialize_scale PROPERTIES
    LABELS "unit;xir;specialization;performance;ownership" TIMEOUT 300)
