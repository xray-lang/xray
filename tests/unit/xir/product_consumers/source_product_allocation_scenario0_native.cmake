# Compile complete emitted C while retaining the original Source0 authority.
set(source_allocation0_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_allocation0_native_root "${CMAKE_BINARY_DIR}/generated/source-allocation0")
set(source_allocation0_native_c "${CMAKE_BINARY_DIR}/generated/source-allocation0-native/program.c")
add_executable(test_source_product_allocation_scenario0_c_emitter
    "${source_allocation0_native_dir}/test_source_product_allocation_scenario0_native.c")
target_link_libraries(test_source_product_allocation_scenario0_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_allocation_scenario0_c_emitter PRIVATE "${source_allocation0_native_dir}/..")
add_custom_command(OUTPUT "${source_allocation0_native_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated/source-allocation0-native"
    COMMAND $<TARGET_FILE:test_source_product_allocation_scenario0_c_emitter>
        "${source_allocation0_native_root}" "${source_allocation0_native_root}/root.xr"
        --emit "${source_allocation0_native_c}"
    DEPENDS test_source_product_allocation_scenario0_c_emitter "${source_allocation0_native_root}/root.xr"
    VERBATIM)
add_executable(test_source_product_allocation_scenario0_native
    "${source_allocation0_native_dir}/test_source_product_allocation_scenario0_native.c" "${source_allocation0_native_c}")
target_compile_definitions(test_source_product_allocation_scenario0_native PRIVATE XR_SOURCE_ALLOCATION0_NATIVE=1)
target_link_libraries(test_source_product_allocation_scenario0_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_allocation_scenario0_native PRIVATE "${source_allocation0_native_dir}/..")
foreach(target IN ITEMS test_source_product_allocation_scenario0_c_emitter test_source_product_allocation_scenario0_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_allocation_scenario0_native_normal
    COMMAND test_source_product_allocation_scenario0_native
        "${source_allocation0_native_root}" "${source_allocation0_native_root}/root.xr")
set_tests_properties(test_source_product_allocation_scenario0_native_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1 LABELS "unit;xir;source-product;program-consumer;ownership;native-projection")
