foreach(_copy_gate refinement flow)
    add_executable(test_xir_source_copy_${_copy_gate}
        ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_copy_${_copy_gate}.c)
    target_include_directories(test_xir_source_copy_${_copy_gate} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(_copy_gate STREQUAL "refinement")
        target_link_libraries(test_xir_source_copy_${_copy_gate} PRIVATE xray_xir xray_xir_scalar xray_xir_source)
    else()
        target_link_libraries(test_xir_source_copy_${_copy_gate} PRIVATE xray_xir_source)
        target_compile_definitions(test_xir_source_copy_${_copy_gate} PRIVATE
            XR_CALLABLE_ROOT_SOURCE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_callable_root_source")
    endif()
    if(MSVC)
        target_compile_options(test_xir_source_copy_${_copy_gate} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_source_copy_${_copy_gate} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_source_copy_${_copy_gate} COMMAND test_xir_source_copy_${_copy_gate})
    set_tests_properties(test_xir_source_copy_${_copy_gate} PROPERTIES
        LABELS "unit;xir;source;root-effects;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
add_test(NAME test_xir_source_copy_refinement_compiler COMMAND test_xir_source_copy_refinement --compiler)
set_tests_properties(test_xir_source_copy_refinement_compiler PROPERTIES
    LABELS "unit;xir;source;root-effects;compiler-fault;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_source_copy_flow_compiler COMMAND test_xir_source_copy_flow --compiler)
set_tests_properties(test_xir_source_copy_flow_compiler PROPERTIES
    LABELS "unit;xir;source;root-effects;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
