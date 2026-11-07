# One multi-library Source program supplies every execution consumer.
set(XIR_LIBRARY_STRING_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_string.xrc)
set(XIR_LIBRARY_STRING_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_string.c)
add_library(xir_library_string_source_runtime OBJECT xir/test_xir_library_string_execution.c)
target_compile_definitions(xir_library_string_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_library_string_source_runtime PRIVATE xray_xir_vm)
add_executable(test_xir_library_string_source
    xir/test_xir_library_string_source.c $<TARGET_OBJECTS:xir_library_string_source_runtime>)
target_link_libraries(test_xir_library_string_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_library_string_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings")
add_custom_command(OUTPUT ${XIR_LIBRARY_STRING_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_library_string_source> ${XIR_LIBRARY_STRING_CHECKED}
    DEPENDS test_xir_library_string_source
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_goldens.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string64_goldens.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_source_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_runtime.h
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/alpha.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/beta.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/private.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/nul_escape.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/nul_unicode.xr
    VERBATIM)

foreach(string_target IN ITEMS xir_library_string_source_runtime test_xir_library_string_source)
    if(MSVC)
        target_compile_options(${string_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${string_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_library_string_source COMMAND test_xir_library_string_source)
add_test(NAME test_xir_library_literal_owner COMMAND test_xir_library_string_source --literal-owner)
set_tests_properties(test_xir_library_literal_owner PROPERTIES LABELS "unit;xir;ownership;fast" TIMEOUT 15)

set_tests_properties(test_xir_library_string_source PROPERTIES LABELS "unit;xir;execution;ownership;abi")
