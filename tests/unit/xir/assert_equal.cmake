# Source, wire, runtime and both native/VM directions use one equality owner.
set(XIR_ASSERT_EQUAL_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-equal-source)
set(XIR_ASSERT_EQUAL_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_equal.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_EQUAL_DIR})
add_xray_bootstrap_executable(test_xir_equal_values ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_equal_values.c)
target_link_libraries(test_xir_equal_values PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_xray_bootstrap_executable(test_xir_assert_equal ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_equal.c)
target_link_libraries(test_xir_assert_equal PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_dependencies(test_xir_assert_equal gen-xir-core-source)
add_custom_command(OUTPUT ${XIR_ASSERT_EQUAL_C}
    COMMAND $<TARGET_FILE:test_xir_assert_equal> ${XIR_ASSERT_EQUAL_DIR} ${XIR_ASSERT_EQUAL_C}
    DEPENDS test_xir_assert_equal
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_equal_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_equal_packet.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_equal_inputs.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_equal_source_oom.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_equal_pipeline_oom.inc.c
    VERBATIM)
add_executable(test_xir_assert_equal_native ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_equal_native.c ${XIR_ASSERT_EQUAL_C})
target_link_libraries(test_xir_assert_equal_native PRIVATE xray_xir_source xray_xir_vm)
foreach(equal_target IN ITEMS test_xir_equal_values test_xir_assert_equal test_xir_assert_equal_native)
    if(MSVC)
        target_compile_options(${equal_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${equal_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_equal_values COMMAND test_xir_equal_values)
add_test(NAME test_xir_assert_equal COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assert-equal-test.c)
add_test(NAME test_xir_assert_equal_allocations COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assert-equal-oom.c oom)
add_test(NAME test_xir_assert_equal_native COMMAND test_xir_assert_equal_native)
add_test(NAME test_xir_assert_equal_existing_fixture COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/assert-equal-fixture.c fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/retired_names_are_ordinary.xr)
add_test(NAME test_xir_equal_builtin_probe COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/assert-equal-probe.c fixture ${PROJECT_SOURCE_DIR}/tests/builtin_probes/equal.xr)
add_test(NAME test_xir_assert_equal_wire_vector COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_equal_vector.py)
add_test(NAME test_xir_equal_wire_migration COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_equal57_migration.py)
set_tests_properties(test_xir_equal_values test_xir_assert_equal test_xir_assert_equal_allocations
    test_xir_assert_equal_native test_xir_assert_equal_existing_fixture test_xir_equal_builtin_probe
    test_xir_assert_equal_wire_vector test_xir_equal_wire_migration
    PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion;equality")
set_tests_properties(test_xir_assert_equal test_xir_assert_equal_allocations PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
