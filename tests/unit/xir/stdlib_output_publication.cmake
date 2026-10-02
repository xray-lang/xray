# The actual io/output module is published before its isolated source is removed.
set(XIR_STDLIB_OUTPUT_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_stdlib_output.c)
add_library(xir_stdlib_output_runtime OBJECT ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_output_execution.c)
target_compile_definitions(xir_stdlib_output_runtime PRIVATE CONSUMER_KIND=0)
target_link_libraries(xir_stdlib_output_runtime PRIVATE xray_xir_vm xray_xir_cgen)
add_xray_bootstrap_executable(test_xir_stdlib_output_source
    ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_output_source.c
    ${CMAKE_CURRENT_LIST_DIR}/xir_stdlib_output_module_probe.c $<TARGET_OBJECTS:xir_stdlib_output_runtime>)
target_link_libraries(test_xir_stdlib_output_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT ${XIR_STDLIB_OUTPUT_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/stdlib_output_publication.py
        $<TARGET_FILE:test_xir_stdlib_output_source> ${CMAKE_SOURCE_DIR} ${XIR_STDLIB_OUTPUT_C}
    DEPENDS test_xir_stdlib_output_source
        ${CMAKE_CURRENT_LIST_DIR}/stdlib_output_publication.py
        ${CMAKE_SOURCE_DIR}/stdlib/io/output.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_stdlib_output/root.xr
    VERBATIM)
add_executable(test_xir_stdlib_output_native ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_output_execution.c ${XIR_STDLIB_OUTPUT_C})
target_compile_definitions(test_xir_stdlib_output_native PRIVATE CONSUMER_KIND=1)
target_link_libraries(test_xir_stdlib_output_native PRIVATE xray_xir_vm)
foreach(publication_target IN ITEMS xir_stdlib_output_runtime test_xir_stdlib_output_source test_xir_stdlib_output_native)
    if(MSVC)
        target_compile_options(${publication_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${publication_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_stdlib_output_source
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/stdlib_output_publication.py
        $<TARGET_FILE:test_xir_stdlib_output_source> ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/stdlib-output-source-test.c)
add_test(NAME test_xir_stdlib_output_native COMMAND test_xir_stdlib_output_native)
set_tests_properties(test_xir_stdlib_output_source test_xir_stdlib_output_native
    PROPERTIES LABELS "unit;xir;execution;ownership;abi")
