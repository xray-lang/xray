# The actual io/output module is published before its isolated source is removed.
set(XIR_STDLIB_OUTPUT_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_stdlib_output.c)
add_library(xir_stdlib_output_runtime OBJECT ${CMAKE_CURRENT_LIST_DIR}/test_xir_stdlib_output_execution.c)
target_compile_definitions(xir_stdlib_output_runtime PRIVATE CONSUMER_KIND=0)
target_link_libraries(xir_stdlib_output_runtime PRIVATE xray_xir_vm xray_xir_cgen)

foreach(publication_target IN ITEMS xir_stdlib_output_runtime)
    if(MSVC)
        target_compile_options(${publication_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${publication_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
