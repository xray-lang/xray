# Actual Source statements initialize before the real exported run executes.
add_executable(test_xir_root_parameter_vm ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_vm.c)
target_include_directories(test_xir_root_parameter_vm PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_root_parameter_vm PRIVATE xray_xir_source xray_xir_vm)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/root_parameter_vm_source")
target_compile_definitions(test_xir_root_parameter_vm PRIVATE
    XR_ROOT_PARAMETER_VM_FIXTURES="${CMAKE_CURRENT_BINARY_DIR}/root_parameter_vm_source")
if(MSVC)
    target_compile_options(test_xir_root_parameter_vm PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_parameter_vm PRIVATE -Wall -Wextra -Werror)
endif()
foreach(_h1_source apply fixed root forward)
    add_test(NAME test_xir_root_parameter_vm_${_h1_source}
        COMMAND test_xir_root_parameter_vm "${_h1_source}.xr")
    set_tests_properties(test_xir_root_parameter_vm_${_h1_source} PROPERTIES
        LABELS "unit;xir;higher-order;source;runtime;ownership;root-effects" TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()

# Census and fallible output retain the same four literal programs.
foreach(_h1_source apply fixed root forward)
    add_test(NAME test_xir_root_parameter_vm_${_h1_source}_census
        COMMAND test_xir_root_parameter_vm --census "${_h1_source}.xr")
    foreach(_h1_failure error limit)
        add_test(NAME test_xir_root_parameter_vm_${_h1_source}_typed_${_h1_failure}
            COMMAND test_xir_root_parameter_vm --typed-failure "${_h1_source}.xr" "${_h1_failure}")
        set_tests_properties(test_xir_root_parameter_vm_${_h1_source}_typed_${_h1_failure} PROPERTIES
            LABELS "unit;xir;higher-order;source;runtime;ownership;output-failure" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
    endforeach()
    set_tests_properties(test_xir_root_parameter_vm_${_h1_source}_census PROPERTIES
        LABELS "unit;xir;higher-order;source;runtime;ownership;resource-census" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
# Strict resource and 64-point OOM commands require an independently frozen oracle.
# Root registers their measured ranges after both provider censuses are audited.
