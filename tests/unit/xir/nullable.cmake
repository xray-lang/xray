# Exact sums share the value, storage, Source, packet and execution owners.
add_executable(test_xir_nullable_values ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_nullable_values.c)
target_link_libraries(test_xir_nullable_values PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
if(MSVC)
    target_compile_options(test_xir_nullable_values PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_nullable_values PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_nullable_values COMMAND test_xir_nullable_values)
add_test(NAME test_xir_nullable_original_same_t COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/nullable-same-t.c fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/same_t_contextual.xr)
set_tests_properties(test_xir_nullable_values test_xir_nullable_original_same_t
    PROPERTIES LABELS "unit;xir;execution;ownership;nullable;abi" TIMEOUT 180)
set(XIR_NULLABLE_SAME_T_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nullable_same_t.c)
add_custom_command(OUTPUT ${XIR_NULLABLE_SAME_T_C}
    COMMAND $<TARGET_FILE:test_xir_assert_equal> ${XIR_ASSERT_EQUAL_DIR} ${XIR_NULLABLE_SAME_T_C} fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/same_t_contextual.xr
    DEPENDS test_xir_assert_equal ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/same_t_contextual.xr
    VERBATIM)
add_executable(test_xir_nullable_native ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_nullable_native.c ${XIR_NULLABLE_SAME_T_C})
target_link_libraries(test_xir_nullable_native PRIVATE xray_xir_source xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_nullable_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_nullable_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_nullable_native COMMAND test_xir_nullable_native)
add_test(NAME test_xir_nullable_wire_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_nullable58_vectors.py)
add_test(NAME test_xir_nullable_allocations COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/nullable-oom.c nullable-oom)
set_tests_properties(test_xir_nullable_allocations
    PROPERTIES LABELS "unit;xir;execution;ownership;nullable;abi" RUN_SERIAL TRUE TIMEOUT 180)
set(XIR_NULLABLE_STORAGE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nullable_storage.c)
add_custom_command(OUTPUT ${XIR_NULLABLE_STORAGE_C}
    COMMAND $<TARGET_FILE:test_xir_assert_equal> ${XIR_ASSERT_EQUAL_DIR} ${XIR_NULLABLE_STORAGE_C} fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/nullable_values.xr
    DEPENDS test_xir_assert_equal ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/nullable_values.xr
    VERBATIM)
add_executable(test_xir_nullable_storage_native ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_nullable_native.c ${XIR_NULLABLE_STORAGE_C})
target_link_libraries(test_xir_nullable_storage_native PRIVATE xray_xir_source xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_nullable_storage_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_xir_nullable_storage_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_nullable_storage_native COMMAND test_xir_nullable_storage_native)
add_test(NAME test_xir_nullable_storage_source COMMAND test_xir_assert_equal ${XIR_ASSERT_EQUAL_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/nullable-storage.c fixture
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/assertion/nullable_values.xr)
set_tests_properties(test_xir_nullable_storage_native test_xir_nullable_storage_source
    PROPERTIES LABELS "unit;xir;execution;ownership;nullable;abi" TIMEOUT 180)
set(XIR_NULLABLE_HOST_ARGS --host-compiler ${CMAKE_C_COMPILER})
if(MSVC OR (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    list(APPEND XIR_NULLABLE_HOST_ARGS --msvc)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    list(APPEND XIR_NULLABLE_HOST_ARGS --msvc-atomics)
endif()
add_test(NAME test_xir_nullable_c11_portability COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/nullable_portability.py ${XIR_NULLABLE_HOST_ARGS}
    --source ${XIR_NULLABLE_SAME_T_C} --source ${XIR_NULLABLE_STORAGE_C})
set_tests_properties(test_xir_nullable_c11_portability
    PROPERTIES LABELS "unit;xir;execution;ownership;nullable;generated-c;portability" TIMEOUT 240)
set_tests_properties(test_xir_nullable_native test_xir_nullable_wire_vectors
    PROPERTIES LABELS "unit;xir;execution;ownership;nullable;abi" TIMEOUT 180)
