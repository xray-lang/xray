# Real Source ownership checks use the existing finite compiler/runtime observers.
add_executable(test_xir_cell_owner_source xir/test_xir_cell_owner_source.c)
target_link_libraries(test_xir_cell_owner_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_cell_owner_source PRIVATE
    XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
add_test(NAME test_xir_cell_owner_source COMMAND test_xir_cell_owner_source)
set_tests_properties(test_xir_cell_owner_source PROPERTIES LABELS "unit;xir;ownership;source" TIMEOUT 120)
# This directed gate exercises the same owned Source/VM pipeline.
add_test(NAME test_xir_class_access_source COMMAND test_xir_cell_owner_source --class-access)
set_tests_properties(test_xir_class_access_source PROPERTIES
    LABELS "unit;xir;ownership;source;class" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)

include(${CMAKE_CURRENT_LIST_DIR}/class_access_source_resources.cmake)
