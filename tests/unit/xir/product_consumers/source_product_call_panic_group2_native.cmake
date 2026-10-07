# Reuse exact complete VM-qualified sealed and indirect call panic sources through native callbacks.
set(call_panic_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(call_panic_native_generated "${CMAKE_BINARY_DIR}/generated/call-panic-native2")
add_executable(test_source_product_call_panic_group2_c_emitter
    "${call_panic_native_dir}/test_source_product_call_panic_group2_native.c")
target_link_libraries(test_source_product_call_panic_group2_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_call_panic_group2_c_emitter PRIVATE "${call_panic_native_dir}/..")
set(call_panic_native_sources)
set(call_panic_native_sealed_call_panic_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/sealed_call_panic")
set(call_panic_native_sealed_call_panic_c "${call_panic_native_generated}/sealed_call_panic.c")
add_custom_command(OUTPUT "${call_panic_native_sealed_call_panic_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${call_panic_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_call_panic_group2_c_emitter> "sealed_call_panic"
        "${call_panic_native_sealed_call_panic_root}" "${call_panic_native_sealed_call_panic_root}/root.xr"
        --emit "${call_panic_native_sealed_call_panic_c}"
    DEPENDS test_source_product_call_panic_group2_c_emitter "${call_panic_native_sealed_call_panic_root}/root.xr"
        "${call_panic_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND call_panic_native_sources "${call_panic_native_sealed_call_panic_c}")
set(call_panic_native_indirect_call_panic_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/indirect_call_panic")
set(call_panic_native_indirect_call_panic_c "${call_panic_native_generated}/indirect_call_panic.c")
add_custom_command(OUTPUT "${call_panic_native_indirect_call_panic_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${call_panic_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_call_panic_group2_c_emitter> "indirect_call_panic"
        "${call_panic_native_indirect_call_panic_root}" "${call_panic_native_indirect_call_panic_root}/root.xr"
        --emit "${call_panic_native_indirect_call_panic_c}"
    DEPENDS test_source_product_call_panic_group2_c_emitter "${call_panic_native_indirect_call_panic_root}/root.xr"
        "${call_panic_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND call_panic_native_sources "${call_panic_native_indirect_call_panic_c}")
add_executable(test_source_product_call_panic_group2_native
    "${call_panic_native_dir}/test_source_product_call_panic_group2_native.c" ${call_panic_native_sources})
target_compile_definitions(test_source_product_call_panic_group2_native PRIVATE XR_SOURCE_CALL_PANIC_GROUP2_NATIVE=1)
target_link_libraries(test_source_product_call_panic_group2_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_call_panic_group2_native PRIVATE "${call_panic_native_dir}/..")
foreach(target IN ITEMS test_source_product_call_panic_group2_c_emitter test_source_product_call_panic_group2_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_call_panic_group2_sealed_call_panic_native_normal COMMAND test_source_product_call_panic_group2_native "sealed_call_panic"
    "${call_panic_native_sealed_call_panic_root}" "${call_panic_native_sealed_call_panic_root}/root.xr")
set_tests_properties(test_source_product_call_panic_group2_sealed_call_panic_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;panic;ownership;native-projection")
add_test(NAME test_source_product_call_panic_group2_indirect_call_panic_native_normal COMMAND test_source_product_call_panic_group2_native "indirect_call_panic"
    "${call_panic_native_indirect_call_panic_root}" "${call_panic_native_indirect_call_panic_root}/root.xr")
set_tests_properties(test_source_product_call_panic_group2_indirect_call_panic_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;panic;ownership;native-projection")
