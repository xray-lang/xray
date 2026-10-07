# Compile the full emitted CAS callbacks under the same private Source authority.
set(atomic_cas_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(atomic_cas_native_fixture "${CMAKE_BINARY_DIR}/generated/atomic-i64-cas-oracles")
set(atomic_cas_native_c "${CMAKE_BINARY_DIR}/generated/atomic-i64-cas-native/program.c")
add_executable(test_source_product_atomic_i64_cas_c_emitter
    "${atomic_cas_native_dir}/test_source_product_atomic_i64_cas_native.c")
target_link_libraries(test_source_product_atomic_i64_cas_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_i64_cas_c_emitter PRIVATE "${atomic_cas_native_dir}/..")
add_custom_command(OUTPUT "${atomic_cas_native_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated/atomic-i64-cas-native"
    COMMAND $<TARGET_FILE:test_source_product_atomic_i64_cas_c_emitter>
        "i64_cas" "${atomic_cas_native_fixture}" "${atomic_cas_native_fixture}/scenario1-cas.xr"
        --emit "${atomic_cas_native_c}"
    DEPENDS test_source_product_atomic_i64_cas_c_emitter "${atomic_cas_native_fixture}/scenario1-cas.xr"
    VERBATIM)
add_executable(test_source_product_atomic_i64_cas_native
    "${atomic_cas_native_dir}/test_source_product_atomic_i64_cas_native.c" "${atomic_cas_native_c}")
target_compile_definitions(test_source_product_atomic_i64_cas_native PRIVATE XR_SOURCE_ATOMIC_CAS_NATIVE=1)
target_link_libraries(test_source_product_atomic_i64_cas_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_i64_cas_native PRIVATE "${atomic_cas_native_dir}/..")
foreach(target IN ITEMS test_source_product_atomic_i64_cas_c_emitter test_source_product_atomic_i64_cas_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_atomic_i64_cas_native_normal COMMAND test_source_product_atomic_i64_cas_native
    "i64_cas" "${atomic_cas_native_fixture}" "${atomic_cas_native_fixture}/scenario1-cas.xr")
set_tests_properties(test_source_product_atomic_i64_cas_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;atomic;ownership;native-projection")
