# Reuse exact complete VM-qualified integer panic sources through native callbacks.
set(integer_intrinsic_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(integer_intrinsic_native_generated "${CMAKE_BINARY_DIR}/generated/source-integer-intrinsic-panic-group3-native")
add_executable(test_source_product_integer_intrinsic_panic_group3_c_emitter
    "${integer_intrinsic_native_dir}/test_source_product_integer_intrinsic_panic_group3_native.c")
target_link_libraries(test_source_product_integer_intrinsic_panic_group3_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_integer_intrinsic_panic_group3_c_emitter PRIVATE "${integer_intrinsic_native_dir}/..")
set(integer_intrinsic_native_sources)
set(integer_intrinsic_native_integer_division_zero_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/integer_division_zero")
set(integer_intrinsic_native_integer_division_zero_c "${integer_intrinsic_native_generated}/integer_division_zero.c")
add_custom_command(OUTPUT "${integer_intrinsic_native_integer_division_zero_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${integer_intrinsic_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_integer_intrinsic_panic_group3_c_emitter> "integer_division_zero"
        "${integer_intrinsic_native_integer_division_zero_root}" "${integer_intrinsic_native_integer_division_zero_root}/root.xr"
        --emit "${integer_intrinsic_native_integer_division_zero_c}"
    DEPENDS test_source_product_integer_intrinsic_panic_group3_c_emitter "${integer_intrinsic_native_integer_division_zero_root}/root.xr"
        "${integer_intrinsic_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND integer_intrinsic_native_sources "${integer_intrinsic_native_integer_division_zero_c}")
set(integer_intrinsic_native_integer_remainder_zero_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/integer_remainder_zero")
set(integer_intrinsic_native_integer_remainder_zero_c "${integer_intrinsic_native_generated}/integer_remainder_zero.c")
add_custom_command(OUTPUT "${integer_intrinsic_native_integer_remainder_zero_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${integer_intrinsic_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_integer_intrinsic_panic_group3_c_emitter> "integer_remainder_zero"
        "${integer_intrinsic_native_integer_remainder_zero_root}" "${integer_intrinsic_native_integer_remainder_zero_root}/root.xr"
        --emit "${integer_intrinsic_native_integer_remainder_zero_c}"
    DEPENDS test_source_product_integer_intrinsic_panic_group3_c_emitter "${integer_intrinsic_native_integer_remainder_zero_root}/root.xr"
        "${integer_intrinsic_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND integer_intrinsic_native_sources "${integer_intrinsic_native_integer_remainder_zero_c}")
set(integer_intrinsic_native_integer_division_defer_root "${CMAKE_BINARY_DIR}/generated/source-integer-division-defer")
set(integer_intrinsic_native_integer_division_defer_c "${integer_intrinsic_native_generated}/integer_division_defer.c")
add_custom_command(OUTPUT "${integer_intrinsic_native_integer_division_defer_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${integer_intrinsic_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_integer_intrinsic_panic_group3_c_emitter> "integer_division_defer"
        "${integer_intrinsic_native_integer_division_defer_root}" "${integer_intrinsic_native_integer_division_defer_root}/root.xr"
        --emit "${integer_intrinsic_native_integer_division_defer_c}"
    DEPENDS test_source_product_integer_intrinsic_panic_group3_c_emitter "${integer_intrinsic_native_integer_division_defer_root}/root.xr"
        "${integer_intrinsic_native_dir}/source_product_integer_division_defer.cmake" VERBATIM)
list(APPEND integer_intrinsic_native_sources "${integer_intrinsic_native_integer_division_defer_c}")
add_executable(test_source_product_integer_intrinsic_panic_group3_native
    "${integer_intrinsic_native_dir}/test_source_product_integer_intrinsic_panic_group3_native.c" ${integer_intrinsic_native_sources})
target_compile_definitions(test_source_product_integer_intrinsic_panic_group3_native PRIVATE XR_SOURCE_INTEGER_INTRINSIC_PANIC_NATIVE=1)
target_link_libraries(test_source_product_integer_intrinsic_panic_group3_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_integer_intrinsic_panic_group3_native PRIVATE "${integer_intrinsic_native_dir}/..")
foreach(target IN ITEMS test_source_product_integer_intrinsic_panic_group3_c_emitter test_source_product_integer_intrinsic_panic_group3_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_integer_intrinsic_panic_group3_integer_division_zero_native_normal COMMAND test_source_product_integer_intrinsic_panic_group3_native "integer_division_zero"
    "${integer_intrinsic_native_integer_division_zero_root}" "${integer_intrinsic_native_integer_division_zero_root}/root.xr")
set_tests_properties(test_source_product_integer_intrinsic_panic_group3_integer_division_zero_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;integer;panic;ownership;native-projection")
add_test(NAME test_source_product_integer_intrinsic_panic_group3_integer_remainder_zero_native_normal COMMAND test_source_product_integer_intrinsic_panic_group3_native "integer_remainder_zero"
    "${integer_intrinsic_native_integer_remainder_zero_root}" "${integer_intrinsic_native_integer_remainder_zero_root}/root.xr")
set_tests_properties(test_source_product_integer_intrinsic_panic_group3_integer_remainder_zero_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;integer;panic;ownership;native-projection")
add_test(NAME test_source_product_integer_intrinsic_panic_group3_integer_division_defer_native_normal COMMAND test_source_product_integer_intrinsic_panic_group3_native "integer_division_defer"
    "${integer_intrinsic_native_integer_division_defer_root}" "${integer_intrinsic_native_integer_division_defer_root}/root.xr")
set_tests_properties(test_source_product_integer_intrinsic_panic_group3_integer_division_defer_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;integer;panic;ownership;native-projection")
