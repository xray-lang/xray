if(TARGET test_lsp_signature_task)
    return()
endif()
source_task_target(test_lsp_signature_task "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c")
source_task_target(test_lsp_signature_task_before "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_navigation.c" "${CMAKE_CURRENT_LIST_DIR}/before_owner.c")
target_compile_definitions(test_lsp_signature_task PRIVATE XLSP_SIGNATURE_TASK=1)
target_compile_definitions(test_lsp_signature_task_before PRIVATE XLSP_SIGNATURE_TASK=1)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_signature_task_before)
    else()
        set(image test_lsp_signature_task)
    endif()
    add_test(NAME test_lsp_signature_task_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_signature.py" "$<TARGET_FILE:${image}>" "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
    set_tests_properties(test_lsp_signature_task_${mode} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership;allocation")
endforeach()
set_tests_properties(test_lsp_signature_task_resources PROPERTIES PROCESSORS 8 COST 290)

foreach(variant 18 20 21 22 24)
    add_test(NAME test_lsp_signature_task_generic_resources_${variant}
        COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_signature.py"
        "$<TARGET_FILE:test_lsp_signature_task>" "${PROJECT_SOURCE_DIR}/stdlib" --generic-resources ${variant})
    set_tests_properties(test_lsp_signature_task_generic_resources_${variant} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 8 COST 290 LABELS "unit;lsp;source;ownership;allocation")
endforeach()
