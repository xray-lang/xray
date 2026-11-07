# Exact sums share the value, storage, Source, packet and execution owners.

set(XIR_NULLABLE_SAME_T_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nullable_same_t.c)

add_test(NAME test_xir_nullable_wire_vectors COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_nullable58_vectors.py)

set(XIR_NULLABLE_STORAGE_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_nullable_storage.c)

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
set_tests_properties(test_xir_nullable_wire_vectors PROPERTIES LABELS "unit;xir;execution;ownership;nullable;abi" TIMEOUT 180)
