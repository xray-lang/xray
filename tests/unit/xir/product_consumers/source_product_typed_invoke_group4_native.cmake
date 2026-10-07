# Reuse four exact VM-qualified typed catch sources through independent native callback processes.
set(typed_invoke_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(typed_invoke_native_generated "${CMAKE_BINARY_DIR}/generated/typed-invoke-native4")
add_executable(test_source_product_typed_invoke_group4_c_emitter
    "${typed_invoke_native_dir}/test_source_product_typed_invoke_group4_native.c")
target_link_libraries(test_source_product_typed_invoke_group4_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_typed_invoke_group4_c_emitter PRIVATE "${typed_invoke_native_dir}/..")
set(typed_invoke_native_sources)
set(typed_invoke_native_sealed_invoke_panic_root "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/sealed_invoke_panic")
set(typed_invoke_native_sealed_invoke_panic_c "${typed_invoke_native_generated}/sealed_invoke_panic.c")
add_custom_command(OUTPUT "${typed_invoke_native_sealed_invoke_panic_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${typed_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_typed_invoke_group4_c_emitter> "sealed_invoke_panic"
        "${typed_invoke_native_sealed_invoke_panic_root}" "${typed_invoke_native_sealed_invoke_panic_root}/root.xr"
        --emit "${typed_invoke_native_sealed_invoke_panic_c}"
    DEPENDS test_source_product_typed_invoke_group4_c_emitter "${typed_invoke_native_sealed_invoke_panic_root}/root.xr"
        "${typed_invoke_native_dir}/source_product_call_error_panic_group5.cmake" VERBATIM)
list(APPEND typed_invoke_native_sources "${typed_invoke_native_sealed_invoke_panic_c}")
set(typed_invoke_native_indirect_invoke_panic_root "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/indirect_invoke_panic")
set(typed_invoke_native_indirect_invoke_panic_c "${typed_invoke_native_generated}/indirect_invoke_panic.c")
add_custom_command(OUTPUT "${typed_invoke_native_indirect_invoke_panic_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${typed_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_typed_invoke_group4_c_emitter> "indirect_invoke_panic"
        "${typed_invoke_native_indirect_invoke_panic_root}" "${typed_invoke_native_indirect_invoke_panic_root}/root.xr"
        --emit "${typed_invoke_native_indirect_invoke_panic_c}"
    DEPENDS test_source_product_typed_invoke_group4_c_emitter "${typed_invoke_native_indirect_invoke_panic_root}/root.xr"
        "${typed_invoke_native_dir}/source_product_call_error_panic_group5.cmake" VERBATIM)
list(APPEND typed_invoke_native_sources "${typed_invoke_native_indirect_invoke_panic_c}")
set(typed_invoke_native_sealed_invoke_defer_root "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/sealed_invoke_defer")
set(typed_invoke_native_sealed_invoke_defer_c "${typed_invoke_native_generated}/sealed_invoke_defer.c")
add_custom_command(OUTPUT "${typed_invoke_native_sealed_invoke_defer_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${typed_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_typed_invoke_group4_c_emitter> "sealed_invoke_defer"
        "${typed_invoke_native_sealed_invoke_defer_root}" "${typed_invoke_native_sealed_invoke_defer_root}/root.xr"
        --emit "${typed_invoke_native_sealed_invoke_defer_c}"
    DEPENDS test_source_product_typed_invoke_group4_c_emitter "${typed_invoke_native_sealed_invoke_defer_root}/root.xr"
        "${typed_invoke_native_dir}/source_product_call_error_panic_group5.cmake" VERBATIM)
list(APPEND typed_invoke_native_sources "${typed_invoke_native_sealed_invoke_defer_c}")
set(typed_invoke_native_indirect_invoke_defer_root "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/indirect_invoke_defer")
set(typed_invoke_native_indirect_invoke_defer_c "${typed_invoke_native_generated}/indirect_invoke_defer.c")
add_custom_command(OUTPUT "${typed_invoke_native_indirect_invoke_defer_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${typed_invoke_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_typed_invoke_group4_c_emitter> "indirect_invoke_defer"
        "${typed_invoke_native_indirect_invoke_defer_root}" "${typed_invoke_native_indirect_invoke_defer_root}/root.xr"
        --emit "${typed_invoke_native_indirect_invoke_defer_c}"
    DEPENDS test_source_product_typed_invoke_group4_c_emitter "${typed_invoke_native_indirect_invoke_defer_root}/root.xr"
        "${typed_invoke_native_dir}/source_product_call_error_panic_group5.cmake" VERBATIM)
list(APPEND typed_invoke_native_sources "${typed_invoke_native_indirect_invoke_defer_c}")
add_executable(test_source_product_typed_invoke_group4_native
    "${typed_invoke_native_dir}/test_source_product_typed_invoke_group4_native.c" ${typed_invoke_native_sources})
target_compile_definitions(test_source_product_typed_invoke_group4_native PRIVATE XR_SOURCE_TYPED_INVOKE_GROUP4_NATIVE=1)
target_link_libraries(test_source_product_typed_invoke_group4_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_typed_invoke_group4_native PRIVATE "${typed_invoke_native_dir}/..")
foreach(target IN ITEMS test_source_product_typed_invoke_group4_c_emitter test_source_product_typed_invoke_group4_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_typed_invoke_group4_sealed_invoke_panic_native_normal COMMAND test_source_product_typed_invoke_group4_native "sealed_invoke_panic"
    "${typed_invoke_native_sealed_invoke_panic_root}" "${typed_invoke_native_sealed_invoke_panic_root}/root.xr")
set_tests_properties(test_source_product_typed_invoke_group4_sealed_invoke_panic_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;typed-catch;panic;ownership;native-projection")
add_test(NAME test_source_product_typed_invoke_group4_indirect_invoke_panic_native_normal COMMAND test_source_product_typed_invoke_group4_native "indirect_invoke_panic"
    "${typed_invoke_native_indirect_invoke_panic_root}" "${typed_invoke_native_indirect_invoke_panic_root}/root.xr")
set_tests_properties(test_source_product_typed_invoke_group4_indirect_invoke_panic_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;typed-catch;panic;ownership;native-projection")
add_test(NAME test_source_product_typed_invoke_group4_sealed_invoke_defer_native_normal COMMAND test_source_product_typed_invoke_group4_native "sealed_invoke_defer"
    "${typed_invoke_native_sealed_invoke_defer_root}" "${typed_invoke_native_sealed_invoke_defer_root}/root.xr")
set_tests_properties(test_source_product_typed_invoke_group4_sealed_invoke_defer_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;typed-catch;panic;ownership;native-projection")
add_test(NAME test_source_product_typed_invoke_group4_indirect_invoke_defer_native_normal COMMAND test_source_product_typed_invoke_group4_native "indirect_invoke_defer"
    "${typed_invoke_native_indirect_invoke_defer_root}" "${typed_invoke_native_indirect_invoke_defer_root}/root.xr")
set_tests_properties(test_source_product_typed_invoke_group4_indirect_invoke_defer_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;call;typed-catch;panic;ownership;native-projection")
