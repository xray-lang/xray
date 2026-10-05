add_executable(test_xir_atomic_a2_source xir/test_xir_atomic_a2_source.c)
target_link_libraries(test_xir_atomic_a2_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_include_directories(test_xir_atomic_a2_source PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(test_xir_atomic_a2_source PRIVATE XR_ATOMIC_A2_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_a2" XR_ATOMIC_A2_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(ATOMIC_A2_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/atomic-a2-native.c")
add_custom_command(OUTPUT "${ATOMIC_A2_NATIVE}"
    COMMAND $<TARGET_FILE:test_xir_atomic_a2_source> "${ATOMIC_A2_NATIVE}"
    DEPENDS test_xir_atomic_a2_source "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_a2/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_a2/state.xr" "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_a2/text.xr"
    VERBATIM)
add_executable(test_xir_atomic_a2_native xir/test_xir_atomic_a2_native.c "${ATOMIC_A2_NATIVE}")
target_link_libraries(test_xir_atomic_a2_native PRIVATE xray_xir_scalar xray_compile_resources)
target_include_directories(test_xir_atomic_a2_native PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
foreach(target test_xir_atomic_a2_source test_xir_atomic_a2_native)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit;xir;ownership;abi;generated-c;execution" TIMEOUT 60)
endforeach()

add_test(NAME test_xir_atomic_a2_compile_owner COMMAND test_xir_atomic_a2_source --faults)
set_tests_properties(test_xir_atomic_a2_compile_owner PROPERTIES LABELS "unit;xir;compiler-memory;ownership;execution" TIMEOUT 300)
