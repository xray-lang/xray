# Reuse the complete VM-qualified finite mutual-call source through native callbacks.
set(recursive_call_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(recursive_call_native_root "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/recursive_call_panic")
set(recursive_call_native_generated "${CMAKE_BINARY_DIR}/generated/recursive-call-panic-native")
set(recursive_call_native_c "${recursive_call_native_generated}/program.c")
add_executable(test_source_product_recursive_call_panic_c_emitter
    "${recursive_call_native_dir}/test_source_product_recursive_call_panic_native.c")
target_link_libraries(test_source_product_recursive_call_panic_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_recursive_call_panic_c_emitter PRIVATE "${recursive_call_native_dir}/..")
add_custom_command(OUTPUT "${recursive_call_native_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${recursive_call_native_generated}"
    COMMAND $<TARGET_FILE:test_source_product_recursive_call_panic_c_emitter>
        "${recursive_call_native_root}" "${recursive_call_native_root}/root.xr" --emit "${recursive_call_native_c}"
    DEPENDS test_source_product_recursive_call_panic_c_emitter "${recursive_call_native_root}/root.xr"
        "${recursive_call_native_dir}/source_product_call_error_panic_group5.cmake" VERBATIM)
add_executable(test_source_product_recursive_call_panic_native
    "${recursive_call_native_dir}/test_source_product_recursive_call_panic_native.c" "${recursive_call_native_c}")
target_compile_definitions(test_source_product_recursive_call_panic_native PRIVATE XR_SOURCE_RECURSIVE_CALL_PANIC_NATIVE=1)
target_link_libraries(test_source_product_recursive_call_panic_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_recursive_call_panic_native PRIVATE "${recursive_call_native_dir}/..")
foreach(target IN ITEMS test_source_product_recursive_call_panic_c_emitter test_source_product_recursive_call_panic_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_recursive_call_panic_native_normal COMMAND test_source_product_recursive_call_panic_native
    "${recursive_call_native_root}" "${recursive_call_native_root}/root.xr")
set_tests_properties(test_source_product_recursive_call_panic_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;recursion;panic;ownership;native-projection")
