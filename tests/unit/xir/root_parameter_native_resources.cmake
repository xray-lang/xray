add_executable(test_xir_root_parameter_native_producer_resources
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_native_producer_resources.c")
target_link_libraries(test_xir_root_parameter_native_producer_resources PRIVATE xray_xir_source xray_xir_cgen)
file(MAKE_DIRECTORY "${H1_NATIVE_DIR}/resource_source")
target_compile_definitions(test_xir_root_parameter_native_producer_resources PRIVATE
    XR_ROOT_PARAMETER_NATIVE_FIXTURES="${H1_NATIVE_DIR}/resource_source")
foreach(_h1_kind native mixed)
    set(_h1_target "test_xir_root_parameter_${_h1_kind}_resources")
    add_executable(${_h1_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_native_resources.c"
        ${H1_NATIVE_C} "${H1_NATIVE_H}")
    target_include_directories(${_h1_target} PRIVATE "${H1_NATIVE_DIR}")
    target_link_libraries(${_h1_target} PRIVATE xray_xir_scalar)
    if(_h1_kind STREQUAL "mixed")
        target_compile_definitions(${_h1_target} PRIVATE H1_MIXED=1)
        target_link_libraries(${_h1_target} PRIVATE xray_xir_vm)
    endif()
    xr_enable_pure_aot_symbol_map(${_h1_target})
endforeach()
foreach(_h1_target test_xir_root_parameter_native_producer_resources test_xir_root_parameter_native_resources test_xir_root_parameter_mixed_resources)
    set_target_properties(${_h1_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${_h1_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${_h1_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(_h1_source apply fixed root forward)
    add_test(NAME test_xir_root_parameter_producer_${_h1_source}_census
        COMMAND test_xir_root_parameter_native_producer_resources --census "${_h1_source}.xr")
    set_tests_properties(test_xir_root_parameter_producer_${_h1_source}_census PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;higher-order;source;generated-c;resource-census")
    foreach(_h1_mode native native-vm vm-native)
        if(_h1_mode STREQUAL "native")
            set(_h1_target test_xir_root_parameter_native_resources)
        else()
            set(_h1_target test_xir_root_parameter_mixed_resources)
        endif()
        string(REPLACE "-" "_" _h1_name "${_h1_mode}")
        add_test(NAME test_xir_root_parameter_${_h1_name}_${_h1_source}_census
            COMMAND ${_h1_target} --census "${_h1_source}.xr" "${_h1_mode}")
        set_tests_properties(test_xir_root_parameter_${_h1_name}_${_h1_source}_census PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;higher-order;generated-c;resource-census")
        foreach(_h1_failure error limit)
            add_test(NAME test_xir_root_parameter_${_h1_name}_${_h1_source}_typed_${_h1_failure}
                COMMAND ${_h1_target} --typed-failure "${_h1_source}.xr" "${_h1_mode}" "${_h1_failure}")
            set_tests_properties(test_xir_root_parameter_${_h1_name}_${_h1_source}_typed_${_h1_failure} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;higher-order;generated-c;output-failure")
        endforeach()
    endforeach()
endforeach()
# Root registers strict external-oracle axes and every actual OOM range after
# auditing fresh producer and consumer censuses for each compiler and mode.
