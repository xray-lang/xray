# The consumer links only the runtime and real generated portable C11.
set(CAP_NATIVE_C "${CMAKE_BINARY_DIR}/generated/xir_capacity_native_contract.c")
set(CAP_NATIVE_H "${CMAKE_BINARY_DIR}/generated/capacity_native_contract_generated.h")
set(CAP_NATIVE_PATH_C "${CMAKE_BINARY_DIR}/generated/xir_capacity_native_path.c")
add_executable(test_xir_array_capacity_native_producer "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_capacity_native_producer.c")
target_link_libraries(test_xir_array_capacity_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_array_capacity_native_producer PRIVATE
    XR_CAP_NATIVE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity_native")
add_custom_command(OUTPUT "${CAP_NATIVE_C}" "${CAP_NATIVE_H}" "${CAP_NATIVE_PATH_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_capacity_native_producer> "${CAP_NATIVE_C}" "${CAP_NATIVE_H}" "${CAP_NATIVE_PATH_C}"
    DEPENDS test_xir_array_capacity_native_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity_native/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity_native/producer.xr" VERBATIM)
add_executable(test_xir_array_capacity_native_contract
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_array_capacity_native_consumer.c" "${CAP_NATIVE_C}" "${CAP_NATIVE_H}" "${CAP_NATIVE_PATH_C}")
target_link_libraries(test_xir_array_capacity_native_contract PRIVATE xray_xir_scalar)
target_include_directories(test_xir_array_capacity_native_contract PRIVATE "${CMAKE_BINARY_DIR}/generated")
xr_enable_pure_aot_symbol_map(test_xir_array_capacity_native_contract)
foreach(target test_xir_array_capacity_native_producer test_xir_array_capacity_native_contract)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_capacity_native_contract COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/verify_array_capacity_native_contract.py"
    --executable $<TARGET_FILE:test_xir_array_capacity_native_contract>)
set_tests_properties(test_xir_array_capacity_native_contract PROPERTIES TIMEOUT 120
    LABELS "unit;xir;array;ownership;execution;generated-c;portability;abi")
