if(TARGET test_lsp_parse_diagnostics)
    return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(mode IN ITEMS normal resources)
    if(mode STREQUAL "before")
        set(image test_lsp_parse_diagnostics_before)
    else()
        set(image test_lsp_parse_diagnostics)
    endif()

endforeach()

if(WIN32)

    if(NOT TARGET test_lsp_source_open)
        add_executable(test_lsp_source_open "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_lsp_source_open.c")
        target_link_libraries(test_lsp_source_open PRIVATE xray_lsp_document_open bcrypt)
        target_compile_options(test_lsp_source_open PRIVATE /W4 /WX /utf-8)
    endif()
    add_test(NAME test_lsp_source_diagnostic_routing_normal COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_lsp_source_open.py" "$<TARGET_FILE:test_lsp_source_open>"
        "${PROJECT_SOURCE_DIR}/stdlib" --diagnostic-routing)
    set_tests_properties(test_lsp_source_diagnostic_routing_normal PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;source;ownership")
endif()
