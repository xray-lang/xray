if(TARGET test_lsp_inlay_source)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(inlay_original "${PROJECT_SOURCE_DIR}/tests/unit/lsp/test_lsp_document.c")
set(inlay_inputs "${CMAKE_CURRENT_BINARY_DIR}/original_inlay_inputs.h")
add_custom_command(OUTPUT "${inlay_inputs}"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_inlay_source.py"
        --generate "${inlay_original}" "${inlay_inputs}"
    DEPENDS "${inlay_original}" "${CMAKE_CURRENT_LIST_DIR}/test_inlay_source.py"
        "${CMAKE_CURRENT_LIST_DIR}/../lsp_config_original/generate_subset.py" VERBATIM)
add_executable(test_lsp_inlay_source "${CMAKE_CURRENT_LIST_DIR}/test_inlay_source.c" "${inlay_inputs}"
    "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_source_bindings.c")
target_include_directories(test_lsp_inlay_source PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_link_libraries(test_lsp_inlay_source PRIVATE xray_lsp_document_open)
target_compile_features(test_lsp_inlay_source PRIVATE c_std_11)
if(WIN32)
    target_link_libraries(test_lsp_inlay_source PRIVATE bcrypt)
endif()
if(MSVC)
    target_compile_options(test_lsp_inlay_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_inlay_source PRIVATE -Wall -Wextra -Werror)
endif()
set(inlay_mode 0)
foreach(case param_modes throw_effects)
    add_test(NAME test_lsp_inlay_source_${case} COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_inlay_source.py" "$<TARGET_FILE:test_lsp_inlay_source>"
        "${PROJECT_SOURCE_DIR}/stdlib" "${inlay_original}" "${inlay_inputs}" "${inlay_mode}")
    set_tests_properties(test_lsp_inlay_source_${case} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;required")
    math(EXPR inlay_mode "${inlay_mode}+1")
endforeach()
