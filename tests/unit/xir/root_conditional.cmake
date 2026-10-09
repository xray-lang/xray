# Root registers these gates after the complete production closure is reviewed.
foreach(_root_conditional_gate helpers source)
    add_executable(test_xir_root_conditional_${_root_conditional_gate}
        ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_conditional_${_root_conditional_gate}.c)
    target_include_directories(test_xir_root_conditional_${_root_conditional_gate} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(_root_conditional_gate STREQUAL "source")
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir_source)
    else()
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir)
    endif()
    if(MSVC)
        target_compile_options(test_xir_root_conditional_${_root_conditional_gate} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_root_conditional_${_root_conditional_gate} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_root_conditional_${_root_conditional_gate}
        COMMAND test_xir_root_conditional_${_root_conditional_gate})
    set_tests_properties(test_xir_root_conditional_${_root_conditional_gate} PROPERTIES
        LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
    add_test(NAME test_xir_root_conditional_${_root_conditional_gate}_compiler
        COMMAND test_xir_root_conditional_${_root_conditional_gate} --compiler)
    set_tests_properties(test_xir_root_conditional_${_root_conditional_gate}_compiler PROPERTIES
        LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
