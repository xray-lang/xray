# Independent qualification uses the same Source pipeline and actual Task/VM hook.
add_executable(test_xir_class_access_source_resources xir/test_xir_class_access_source_resources.c)
target_link_libraries(test_xir_class_access_source_resources PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_class_access_source_resources PRIVATE
    XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
foreach(mode IN ITEMS normal compiler-fi compiler-axes runtime-fi runtime-axes runtime-prepare runtime-control)
    string(REPLACE "-" "_" suffix "${mode}")
    add_test(NAME test_xir_class_access_source_${suffix}
        COMMAND test_xir_class_access_source_resources --${mode})
    set_tests_properties(test_xir_class_access_source_${suffix} PROPERTIES
        LABELS "unit;xir;ownership;source;class;resources" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/class_access_native.cmake)
