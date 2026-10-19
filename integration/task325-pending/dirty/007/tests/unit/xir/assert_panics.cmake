# Both emitted programs retain the same checked proof and typed default owner.
set(XIR_ASSERT_PANICS_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/assert-panics-source)
set(XIR_ASSERT_PANICS_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_assert_panics.c)
set(XIR_ASSERT_PANICS_MATRIX_C ${XIR_ASSERT_PANICS_C}.matrix.c)
file(MAKE_DIRECTORY ${XIR_ASSERT_PANICS_DIR})
add_executable(test_xir_assert_panics ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_panics.c)
target_link_libraries(test_xir_assert_panics PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_dependencies(test_xir_assert_panics gen-xir-core-source)
add_custom_command(OUTPUT ${XIR_ASSERT_PANICS_C} ${XIR_ASSERT_PANICS_MATRIX_C}
    COMMAND $<TARGET_FILE:test_xir_assert_panics> ${XIR_ASSERT_PANICS_DIR} ${XIR_ASSERT_PANICS_C}
    DEPENDS test_xir_assert_panics
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics64_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics65_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics69_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics70_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics71_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_inbox_gates.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_execution.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_packet.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_authority.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_source_oom.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_pipeline_oom.inc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_assert_panics_resource.inc.c
    VERBATIM)
add_executable(test_xir_assert_panics_native ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_assert_panics_native.c
    ${XIR_ASSERT_PANICS_C} ${XIR_ASSERT_PANICS_MATRIX_C})
target_link_libraries(test_xir_assert_panics_native PRIVATE xray_xir_source xray_xir_vm)
foreach(panics_target IN ITEMS test_xir_assert_panics test_xir_assert_panics_native)
    if(MSVC)
        target_compile_options(${panics_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${panics_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_assert_panics COMMAND test_xir_assert_panics
    ${XIR_ASSERT_PANICS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assert-panics-test.c)
add_test(NAME test_xir_assert_panics_allocations COMMAND test_xir_assert_panics
    ${XIR_ASSERT_PANICS_DIR} ${CMAKE_CURRENT_BINARY_DIR}/assert-panics-allocations.c oom)
add_test(NAME test_xir_assert_panics_native COMMAND test_xir_assert_panics_native)
add_test(NAME test_xir_assert_panics_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_assert_panics_vector.py)
add_test(NAME test_xir_assert_panics_current_wire_vector COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_construction72_vectors.py
        --source-root ${CMAKE_SOURCE_DIR} --output-root ${CMAKE_SOURCE_DIR})
add_test(NAME test_xir_binder_wire_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_binder56_migration.py)
set_tests_properties(test_xir_assert_panics test_xir_assert_panics_allocations test_xir_assert_panics_native
    test_xir_assert_panics_wire_vector test_xir_assert_panics_current_wire_vector test_xir_binder_wire_vectors PROPERTIES LABELS "unit;xir;execution;ownership;abi;assertion")
set_tests_properties(test_xir_assert_panics test_xir_assert_panics_allocations PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)

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
