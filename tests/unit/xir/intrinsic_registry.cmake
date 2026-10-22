add_executable(test_xir_intrinsic_registry xir/test_xir_intrinsic_registry.c)
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
foreach(target test_xir_intrinsic_registry test_xir_intrinsic_registry_native)
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
add_test(NAME xir_intrinsic_contains COMMAND test_xir_intrinsic_registry)
add_test(NAME xir_intrinsic_contains_native COMMAND test_xir_intrinsic_registry_native)
set_tests_properties(xir_intrinsic_contains xir_intrinsic_contains_native PROPERTIES
    LABELS "unit;xir;intrinsic;fast" TIMEOUT 60)
