# Compile each complete Atomic family with independent generated ProgramSpec identities.
set(atomic_families_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(atomic_families_native_fixture "${CMAKE_BINARY_DIR}/generated/source-atomic-families")
set(atomic_families_native_generated "${CMAKE_BINARY_DIR}/generated/atomic-source-families-native")
add_executable(test_source_product_atomic_source_families_c_emitter
    "${atomic_families_native_dir}/test_source_product_atomic_source_families_native.c")
target_link_libraries(test_source_product_atomic_source_families_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_source_families_c_emitter PRIVATE "${atomic_families_native_dir}/..")
set(atomic_families_native_sources)
foreach(scenario IN ITEMS 1 2 3)
    set(generated_c "${atomic_families_native_generated}/scenario${scenario}.c")
    set(source_fixture "${atomic_families_native_fixture}/scenario${scenario}-atomic.xr")
    add_custom_command(OUTPUT "${generated_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${atomic_families_native_generated}"
        COMMAND $<TARGET_FILE:test_source_product_atomic_source_families_c_emitter>
            "${scenario}" "atomic" "${atomic_families_native_fixture}" "${source_fixture}"
            --emit "${generated_c}"
        DEPENDS test_source_product_atomic_source_families_c_emitter "${source_fixture}"
        VERBATIM)
    list(APPEND atomic_families_native_sources "${generated_c}")
endforeach()
add_executable(test_source_product_atomic_source_families_native
    "${atomic_families_native_dir}/test_source_product_atomic_source_families_native.c"
    ${atomic_families_native_sources})
target_compile_definitions(test_source_product_atomic_source_families_native PRIVATE XR_SOURCE_ATOMIC_FAMILIES_NATIVE=1)
target_link_libraries(test_source_product_atomic_source_families_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_source_families_native PRIVATE "${atomic_families_native_dir}/..")
foreach(target IN ITEMS test_source_product_atomic_source_families_c_emitter test_source_product_atomic_source_families_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(scenario IN ITEMS 1 2 3)
    set(test_name "test_source_product_atomic_source${scenario}_native_normal")
    add_test(NAME ${test_name} COMMAND test_source_product_atomic_source_families_native
        "${scenario}" "atomic" "${atomic_families_native_fixture}"
        "${atomic_families_native_fixture}/scenario${scenario}-atomic.xr")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;atomic;ownership;native-projection")
endforeach()
