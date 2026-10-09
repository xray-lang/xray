# Root owns the including Unit registry. This include owns only new H1 targets.
foreach(_root_parameter_gate contract resources instances wire visibility lower_names capture_index)
    add_executable(test_xir_root_parameter_${_root_parameter_gate}
        ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_${_root_parameter_gate}.c)
    target_include_directories(test_xir_root_parameter_${_root_parameter_gate} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_link_libraries(test_xir_root_parameter_${_root_parameter_gate} PRIVATE xray_xir)
    if(MSVC)
        target_compile_options(test_xir_root_parameter_${_root_parameter_gate} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_root_parameter_${_root_parameter_gate} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_root_parameter_${_root_parameter_gate} COMMAND test_xir_root_parameter_${_root_parameter_gate})
    set_tests_properties(test_xir_root_parameter_${_root_parameter_gate} PROPERTIES
        LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
target_link_libraries(test_xir_root_parameter_visibility PRIVATE xray_xir_vm)
add_executable(test_xir_root_parameter_source ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_source.c)
target_include_directories(test_xir_root_parameter_source PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_root_parameter_source PRIVATE xray_xir_source)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/root_parameter_source")
target_compile_definitions(test_xir_root_parameter_source PRIVATE
    XR_ROOT_PARAMETER_SOURCE_FIXTURES="${CMAKE_CURRENT_BINARY_DIR}/root_parameter_source")
if(MSVC)
    target_compile_options(test_xir_root_parameter_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_root_parameter_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_root_parameter_source COMMAND test_xir_root_parameter_source)
set_tests_properties(test_xir_root_parameter_source PROPERTIES
    LABELS "unit;xir;source;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_root_parameter_source_compiler COMMAND test_xir_root_parameter_source --compiler)
set_tests_properties(test_xir_root_parameter_source_compiler PROPERTIES
    LABELS "unit;xir;source;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
