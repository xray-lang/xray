add_executable(test_xir_intrinsic_registry xir/test_xir_intrinsic_registry.c)
add_executable(test_xir_string_bytes xir/test_xir_string_bytes.c)
target_link_libraries(test_xir_string_bytes PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(string_bytes_fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/string_bytes_consumers.xr")
set(string_bytes_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/string_bytes.c")
set(string_bytes_packet "${CMAKE_CURRENT_BINARY_DIR}/generated/string_bytes.xrc")
add_custom_command(OUTPUT "${string_bytes_generated}" "${string_bytes_packet}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_string_bytes> --emit
        "${string_bytes_fixture}" "${string_bytes_generated}" "${string_bytes_packet}"
    DEPENDS test_xir_string_bytes "${string_bytes_fixture}"
    VERBATIM)
add_executable(test_xir_string_bytes_native
    xir/test_xir_string_bytes_native.c "${string_bytes_generated}")
target_link_libraries(test_xir_string_bytes_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_link_libraries(test_xir_intrinsic_registry PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_lir_lower)
set(intrinsic_contains_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/intrinsic_contains.c")
add_custom_command(OUTPUT "${intrinsic_contains_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_intrinsic_registry> --emit "${intrinsic_contains_generated}"
    DEPENDS test_xir_intrinsic_registry
    VERBATIM)
add_executable(test_xir_intrinsic_registry_native
    xir/test_xir_intrinsic_registry_native.c "${intrinsic_contains_generated}")
target_link_libraries(test_xir_intrinsic_registry_native PRIVATE xray_xir_scalar xray_compile_resources)
foreach(target test_xir_intrinsic_registry test_xir_intrinsic_registry_native
    test_xir_string_bytes test_xir_string_bytes_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME xir_intrinsic_contains COMMAND test_xir_intrinsic_registry
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/contains-schema33.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/scalar-schema33.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/predicates-schema34.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/scalar-schema34.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/evaluation-contract94.xrc")
add_test(NAME xir_intrinsic_contains_native COMMAND test_xir_intrinsic_registry_native)
set_tests_properties(xir_intrinsic_contains xir_intrinsic_contains_native PROPERTIES
    LABELS "unit;xir;intrinsic;fast" TIMEOUT 60)
add_test(NAME xir_string_bytes COMMAND test_xir_string_bytes
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/string_bytes_consumers.xr")
add_test(NAME xir_string_bytes_packet COMMAND test_xir_string_bytes --packet "${string_bytes_packet}")
add_test(NAME xir_string_bytes_native COMMAND test_xir_string_bytes_native --native "${string_bytes_packet}")
add_test(NAME xir_string_bytes_native_vm COMMAND test_xir_string_bytes_native --native-vm "${string_bytes_packet}")
add_test(NAME xir_string_bytes_vm_native COMMAND test_xir_string_bytes_native --vm-native "${string_bytes_packet}")
set_tests_properties(xir_string_bytes PROPERTIES LABELS "unit;xir;intrinsic" TIMEOUT 60)
set_tests_properties(xir_string_bytes_packet xir_string_bytes_native
    xir_string_bytes_native_vm xir_string_bytes_vm_native PROPERTIES LABELS "unit;xir;intrinsic;native" TIMEOUT 60)
