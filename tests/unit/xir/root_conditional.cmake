# Root registers these gates after the complete production closure is reviewed.
set(XRAY_ROOT_CONDITIONAL_COMPILER_REPORTS_ROOT
    "${CMAKE_BINARY_DIR}/root-conditional-source-compiler" CACHE PATH
    "Exclusive reports root for complete conditional Source compiler faults")
foreach(_root_conditional_gate helpers source)
    add_executable(test_xir_root_conditional_${_root_conditional_gate}
        ${CMAKE_CURRENT_LIST_DIR}/test_xir_root_conditional_${_root_conditional_gate}.c)
    target_include_directories(test_xir_root_conditional_${_root_conditional_gate} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(_root_conditional_gate STREQUAL "source")
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir_source)
    else()
        target_link_libraries(test_xir_root_conditional_${_root_conditional_gate} PRIVATE xray_xir xray_xir_scalar)
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
    # Observed Windows wall costs: complete MSVC 110s; ASan reaches its 600s cap.
    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        set(_root_source_compiler_cost 600.113)
    else()
        set(_root_source_compiler_cost 110.006)
    endif()
    if(WIN32 AND _root_conditional_gate STREQUAL "source")
        add_test(NAME test_xir_root_conditional_${_root_conditional_gate}_compiler
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/xir/run_root_conditional_source_compiler.py"
                --executable "$<TARGET_FILE:test_xir_root_conditional_source>"
                --source-root "${PROJECT_SOURCE_DIR}"
                --reports-root "${XRAY_ROOT_CONDITIONAL_COMPILER_REPORTS_ROOT}" --workers 8)
        set_tests_properties(test_xir_root_conditional_${_root_conditional_gate}_compiler PROPERTIES
            LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 8 COST ${_root_source_compiler_cost})
    else()
        add_test(NAME test_xir_root_conditional_${_root_conditional_gate}_compiler
            COMMAND test_xir_root_conditional_${_root_conditional_gate} --compiler)
        set_tests_properties(test_xir_root_conditional_${_root_conditional_gate}_compiler PROPERTIES
            LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
    endif()
endforeach()

# Existing producer identities must follow actual Source normalization.
add_executable(test_xir_source_effect_value_refine
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_effect_value_refine.c)
target_include_directories(test_xir_source_effect_value_refine PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_effect_value_refine PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_effect_value_refine PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_effect_value_refine PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_effect_value_refine COMMAND test_xir_source_effect_value_refine)
set_tests_properties(test_xir_source_effect_value_refine PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_source_effect_value_refine_compiler
    COMMAND test_xir_source_effect_value_refine --compiler)
set_tests_properties(test_xir_source_effect_value_refine_compiler PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)

# A genuine open Fn(A)->B is checked in its own complete generic definition.
add_executable(test_xir_source_generic_callable_context
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_generic_callable_context.c)
target_include_directories(test_xir_source_generic_callable_context PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_generic_callable_context PRIVATE xray_xir_source)
if(MSVC)
    target_compile_options(test_xir_source_generic_callable_context PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_source_generic_callable_context PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_generic_callable_context COMMAND test_xir_source_generic_callable_context)
set_tests_properties(test_xir_source_generic_callable_context PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler;ownership" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
add_test(NAME test_xir_source_generic_callable_context_compiler
    COMMAND test_xir_source_generic_callable_context --compiler)
set_tests_properties(test_xir_source_generic_callable_context_compiler PROPERTIES
    LABELS "unit;xir;root-effects;higher-order;compiler-fault;ownership" TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
