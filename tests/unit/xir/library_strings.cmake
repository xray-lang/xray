# One multi-library Source program supplies every execution consumer.
set(XIR_LIBRARY_STRING_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_string.xrc)
set(XIR_LIBRARY_STRING_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_string.c)
add_library(xir_library_string_source_runtime OBJECT xir/test_xir_library_string_execution.c)
target_compile_definitions(xir_library_string_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_library_string_source_runtime PRIVATE xray_xir_vm)
add_xray_bootstrap_executable(test_xir_library_string_source
    xir/test_xir_library_string_source.c $<TARGET_OBJECTS:xir_library_string_source_runtime>)
target_link_libraries(test_xir_library_string_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_library_string_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings")
add_custom_command(OUTPUT ${XIR_LIBRARY_STRING_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_library_string_source> ${XIR_LIBRARY_STRING_CHECKED}
    DEPENDS test_xir_library_string_source
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_goldens.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_source_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_string_runtime.h
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/alpha.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/beta.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/private.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/nul_escape.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_strings/nul_unicode.xr
    VERBATIM)
add_executable(test_xir_library_string_packet
    xir/test_xir_library_string_execution.c ${XIR_LIBRARY_STRING_CHECKED})
target_compile_definitions(test_xir_library_string_packet PRIVATE CONSUMER_KIND=0)
target_link_libraries(test_xir_library_string_packet PRIVATE xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT ${XIR_LIBRARY_STRING_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_library_string_packet>
        ${XIR_LIBRARY_STRING_CHECKED} ${XIR_LIBRARY_STRING_GENERATED}
    DEPENDS test_xir_library_string_packet ${XIR_LIBRARY_STRING_CHECKED}
    VERBATIM)
foreach(string_mode IN ITEMS native mixed)
    add_executable(test_xir_library_string_${string_mode}
        xir/test_xir_library_string_execution.c ${XIR_LIBRARY_STRING_GENERATED})
    target_link_libraries(test_xir_library_string_${string_mode} PRIVATE xray_xir_vm)
    if(string_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_library_string_${string_mode} PRIVATE CONSUMER_KIND=2)
    else()
        target_compile_definitions(test_xir_library_string_${string_mode} PRIVATE CONSUMER_KIND=1)
    endif()
endforeach()
foreach(string_target IN ITEMS xir_library_string_source_runtime
        test_xir_library_string_source test_xir_library_string_packet
        test_xir_library_string_native test_xir_library_string_mixed)
    if(MSVC)
        target_compile_options(${string_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${string_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_library_string_source COMMAND test_xir_library_string_source)
add_test(NAME test_xir_library_string_packet COMMAND test_xir_library_string_packet
    ${XIR_LIBRARY_STRING_CHECKED} ${CMAKE_CURRENT_BINARY_DIR}/library-string-packet-test.c)
add_test(NAME test_xir_library_string_native COMMAND test_xir_library_string_native)
add_test(NAME test_xir_library_string_mixed COMMAND test_xir_library_string_mixed)
set_tests_properties(test_xir_library_string_source test_xir_library_string_packet
    test_xir_library_string_native test_xir_library_string_mixed
    PROPERTIES LABELS "unit;xir;execution;ownership;abi")
