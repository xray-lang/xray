# Reuse complete VM-qualified typed coroutine invoke sources with nominal catches and conditional cleanup through native callbacks.
set(coroutine_invoke_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(coroutine_invoke_native_generated "${CMAKE_BINARY_DIR}/generated/coroutine-invoke-native2")
add_executable(test_source_product_coroutine_invoke_group2_c_emitter
    "${coroutine_invoke_native_dir}/test_source_product_coroutine_invoke_group2_native.c")
target_link_libraries(test_source_product_coroutine_invoke_group2_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_coroutine_invoke_group2_c_emitter PRIVATE "${coroutine_invoke_native_dir}/..")
set(coroutine_invoke_native_sources)
set(coroutine_invoke_native_sealed_coroutine_invoke_root "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/sealed_coroutine_invoke")
set(coroutine_invoke_native_sealed_coroutine_invoke_c "${coroutine_invoke_native_generated}/sealed_coroutine_invoke.c")
add_custom_command(OUTPUT "${coroutine_invoke_native_sealed_coroutine_invoke_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${coroutine_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_coroutine_invoke_group2_c_emitter> "sealed_coroutine_invoke"
        "${coroutine_invoke_native_sealed_coroutine_invoke_root}" "${coroutine_invoke_native_sealed_coroutine_invoke_root}/root.xr"
        --emit "${coroutine_invoke_native_sealed_coroutine_invoke_c}"
    DEPENDS test_source_product_coroutine_invoke_group2_c_emitter "${coroutine_invoke_native_sealed_coroutine_invoke_root}/root.xr"
        "${coroutine_invoke_native_dir}/source_product_coroutine_panic_group4.cmake" VERBATIM)
list(APPEND coroutine_invoke_native_sources "${coroutine_invoke_native_sealed_coroutine_invoke_c}")
set(coroutine_invoke_native_indirect_coroutine_invoke_root "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/indirect_coroutine_invoke")
set(coroutine_invoke_native_indirect_coroutine_invoke_c "${coroutine_invoke_native_generated}/indirect_coroutine_invoke.c")
add_custom_command(OUTPUT "${coroutine_invoke_native_indirect_coroutine_invoke_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${coroutine_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_coroutine_invoke_group2_c_emitter> "indirect_coroutine_invoke"
        "${coroutine_invoke_native_indirect_coroutine_invoke_root}" "${coroutine_invoke_native_indirect_coroutine_invoke_root}/root.xr"
        --emit "${coroutine_invoke_native_indirect_coroutine_invoke_c}"
    DEPENDS test_source_product_coroutine_invoke_group2_c_emitter "${coroutine_invoke_native_indirect_coroutine_invoke_root}/root.xr"
        "${coroutine_invoke_native_dir}/source_product_coroutine_panic_group4.cmake" VERBATIM)
list(APPEND coroutine_invoke_native_sources "${coroutine_invoke_native_indirect_coroutine_invoke_c}")
add_executable(test_source_product_coroutine_invoke_group2_native
    "${coroutine_invoke_native_dir}/test_source_product_coroutine_invoke_group2_native.c" ${coroutine_invoke_native_sources})
target_compile_definitions(test_source_product_coroutine_invoke_group2_native PRIVATE XR_SOURCE_COROUTINE_INVOKE_NATIVE=1)
target_link_libraries(test_source_product_coroutine_invoke_group2_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_coroutine_invoke_group2_native PRIVATE "${coroutine_invoke_native_dir}/..")
foreach(target IN ITEMS test_source_product_coroutine_invoke_group2_c_emitter test_source_product_coroutine_invoke_group2_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_coroutine_invoke_group2_sealed_coroutine_invoke_native_normal COMMAND test_source_product_coroutine_invoke_group2_native "sealed_coroutine_invoke"
    "${coroutine_invoke_native_sealed_coroutine_invoke_root}" "${coroutine_invoke_native_sealed_coroutine_invoke_root}/root.xr")
set_tests_properties(test_source_product_coroutine_invoke_group2_sealed_coroutine_invoke_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;coroutine;typed-catch;panic;defer;ownership;native-projection")
add_test(NAME test_source_product_coroutine_invoke_group2_indirect_coroutine_invoke_native_normal COMMAND test_source_product_coroutine_invoke_group2_native "indirect_coroutine_invoke"
    "${coroutine_invoke_native_indirect_coroutine_invoke_root}" "${coroutine_invoke_native_indirect_coroutine_invoke_root}/root.xr")
set_tests_properties(test_source_product_coroutine_invoke_group2_indirect_coroutine_invoke_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;coroutine;typed-catch;panic;defer;ownership;native-projection")
