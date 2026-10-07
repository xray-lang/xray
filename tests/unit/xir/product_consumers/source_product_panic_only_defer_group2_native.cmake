# Reuse exact complete VM-qualified sealed and indirect call panic plus conditional defer sources through native callbacks.
set(panic_only_defer_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(panic_only_defer_native_generated "${CMAKE_BINARY_DIR}/generated/panic-only-defer-native2")
add_executable(test_source_product_panic_only_defer_group2_c_emitter
    "${panic_only_defer_native_dir}/test_source_product_panic_only_defer_group2_native.c")
target_link_libraries(test_source_product_panic_only_defer_group2_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_panic_only_defer_group2_c_emitter PRIVATE "${panic_only_defer_native_dir}/..")
set(panic_only_defer_native_sources)
set(panic_only_defer_native_sealed_panic_only_defer_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/sealed_panic_only_defer")
set(panic_only_defer_native_sealed_panic_only_defer_c "${panic_only_defer_native_generated}/sealed_panic_only_defer.c")
add_custom_command(OUTPUT "${panic_only_defer_native_sealed_panic_only_defer_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${panic_only_defer_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_panic_only_defer_group2_c_emitter> "sealed_panic_only_defer"
        "${panic_only_defer_native_sealed_panic_only_defer_root}" "${panic_only_defer_native_sealed_panic_only_defer_root}/root.xr"
        --emit "${panic_only_defer_native_sealed_panic_only_defer_c}"
    DEPENDS test_source_product_panic_only_defer_group2_c_emitter "${panic_only_defer_native_sealed_panic_only_defer_root}/root.xr"
        "${panic_only_defer_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND panic_only_defer_native_sources "${panic_only_defer_native_sealed_panic_only_defer_c}")
set(panic_only_defer_native_indirect_panic_only_defer_root "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/indirect_panic_only_defer")
set(panic_only_defer_native_indirect_panic_only_defer_c "${panic_only_defer_native_generated}/indirect_panic_only_defer.c")
add_custom_command(OUTPUT "${panic_only_defer_native_indirect_panic_only_defer_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${panic_only_defer_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_panic_only_defer_group2_c_emitter> "indirect_panic_only_defer"
        "${panic_only_defer_native_indirect_panic_only_defer_root}" "${panic_only_defer_native_indirect_panic_only_defer_root}/root.xr"
        --emit "${panic_only_defer_native_indirect_panic_only_defer_c}"
    DEPENDS test_source_product_panic_only_defer_group2_c_emitter "${panic_only_defer_native_indirect_panic_only_defer_root}/root.xr"
        "${panic_only_defer_native_dir}/source_product_builtin_panic_group6.cmake" VERBATIM)
list(APPEND panic_only_defer_native_sources "${panic_only_defer_native_indirect_panic_only_defer_c}")
add_executable(test_source_product_panic_only_defer_group2_native
    "${panic_only_defer_native_dir}/test_source_product_panic_only_defer_group2_native.c" ${panic_only_defer_native_sources})
target_compile_definitions(test_source_product_panic_only_defer_group2_native PRIVATE XR_SOURCE_PANIC_ONLY_DEFER_GROUP2_NATIVE=1)
target_link_libraries(test_source_product_panic_only_defer_group2_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_panic_only_defer_group2_native PRIVATE "${panic_only_defer_native_dir}/..")
foreach(target IN ITEMS test_source_product_panic_only_defer_group2_c_emitter test_source_product_panic_only_defer_group2_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_panic_only_defer_group2_sealed_panic_only_defer_native_normal COMMAND test_source_product_panic_only_defer_group2_native "sealed_panic_only_defer"
    "${panic_only_defer_native_sealed_panic_only_defer_root}" "${panic_only_defer_native_sealed_panic_only_defer_root}/root.xr")
set_tests_properties(test_source_product_panic_only_defer_group2_sealed_panic_only_defer_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;panic;defer;ownership;native-projection")
add_test(NAME test_source_product_panic_only_defer_group2_indirect_panic_only_defer_native_normal COMMAND test_source_product_panic_only_defer_group2_native "indirect_panic_only_defer"
    "${panic_only_defer_native_indirect_panic_only_defer_root}" "${panic_only_defer_native_indirect_panic_only_defer_root}/root.xr")
set_tests_properties(test_source_product_panic_only_defer_group2_indirect_panic_only_defer_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;panic;defer;ownership;native-projection")
