

add_executable(test_xir_source_refinement xir/test_xir_source_refinement.c)
target_link_libraries(test_xir_source_refinement PRIVATE xray_xir_source)
target_include_directories(test_xir_source_refinement PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
add_dependencies(test_xir_source_refinement gen-xir-core-source gen-stdlib-embedded-sources)
if(MSVC)
    target_compile_options(test_xir_source_refinement PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_refinement PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_refinement COMMAND test_xir_source_refinement)
set_tests_properties(test_xir_source_refinement PROPERTIES LABELS "unit;xir;metadata;ownership" TIMEOUT 120)
add_test(NAME test_xir_tuple62_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple62_vectors.py)
add_test(NAME test_xir_tuple63_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple63_vectors.py)
add_test(NAME test_xir_tuple63_readers COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/verify_tuple63_readers.py
    --reader $<TARGET_FILE:test_xir_nested_nullable_reader> --output ${CMAKE_CURRENT_BINARY_DIR}/tuple63-reader-probes)
add_test(NAME test_xir_tuple63_literal_projection COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple63_migration.py)
set_tests_properties(test_xir_tuple62_vectors test_xir_tuple63_vectors test_xir_tuple63_readers test_xir_tuple63_literal_projection PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)

add_test(NAME test_xir_tuple62_sizing_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_tuple62_migration.py)
set_tests_properties(test_xir_tuple62_sizing_vectors PROPERTIES LABELS "unit;xir;abi;ownership" TIMEOUT 300)
