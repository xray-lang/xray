# Reuse complete qualified default assertion sources through native callbacks.
set(default_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(default_native_fixture "${CMAKE_BINARY_DIR}/generated/source-default-assertion-group2")
set(default_native_generated "${CMAKE_BINARY_DIR}/generated/source-default-assertion-group2-native")
add_executable(test_source_product_default_assertion_group2_c_emitter
    "${default_native_dir}/test_source_product_default_assertion_group2_native.c")
target_link_libraries(test_source_product_default_assertion_group2_c_emitter PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_default_assertion_group2_c_emitter PRIVATE "${default_native_dir}/..")
set(default_native_sources)
foreach(case IN ITEMS assertion_owner_cleanup assertion_defer)
    set(case_root "${default_native_fixture}/${case}")
    set(generated_c "${default_native_generated}/${case}.c")
    add_custom_command(OUTPUT "${generated_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${default_native_generated}"
        COMMAND $<TARGET_FILE:test_source_product_default_assertion_group2_c_emitter>
            "${case}" "${case_root}" "${case_root}/root.xr" --emit "${generated_c}"
        DEPENDS test_source_product_default_assertion_group2_c_emitter "${case_root}/root.xr"
            "${default_native_dir}/source_product_default_assertion_group2.cmake"
        VERBATIM)
    list(APPEND default_native_sources "${generated_c}")
endforeach()
add_executable(test_source_product_default_assertion_group2_native
    "${default_native_dir}/test_source_product_default_assertion_group2_native.c" ${default_native_sources})
target_compile_definitions(test_source_product_default_assertion_group2_native PRIVATE XR_SOURCE_DEFAULT_ASSERTION_NATIVE=1)
target_link_libraries(test_source_product_default_assertion_group2_native PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_default_assertion_group2_native PRIVATE "${default_native_dir}/..")
foreach(target IN ITEMS test_source_product_default_assertion_group2_c_emitter test_source_product_default_assertion_group2_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(case IN ITEMS assertion_owner_cleanup assertion_defer)
    set(test_name "test_source_product_default_assertion_group2_${case}_native_normal")
    add_test(NAME ${test_name} COMMAND test_source_product_default_assertion_group2_native
        "${case}" "${default_native_fixture}/${case}" "${default_native_fixture}/${case}/root.xr")
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;assertion;ownership;native-projection")
endforeach()
