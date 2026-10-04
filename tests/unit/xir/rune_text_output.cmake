# The original native text target keeps its fixed-byte and symbol-map oracle.
set(XIR_TEXT_OUTPUT_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_text_output)
set(XIR_TEXT_OUTPUT_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_text_output.c)
add_executable(test_xir_text_output_source xir/test_xir_text_output_source.c)
target_link_libraries(test_xir_text_output_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_text_output_source PRIVATE
    XR_TEXT_OUTPUT_FIXTURES="${XIR_TEXT_OUTPUT_FIXTURE}"
    XR_TEXT_OUTPUT_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_TEXT_OUTPUT_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_text_output_source> ${XIR_TEXT_OUTPUT_GENERATED}
    DEPENDS test_xir_text_output_source ${XIR_TEXT_OUTPUT_FIXTURE}/root.xr
    VERBATIM)
add_executable(test_xr_program_aot_text_output
    xir/test_xir_text_output_native.c ${XIR_TEXT_OUTPUT_GENERATED})
target_link_libraries(test_xr_program_aot_text_output PRIVATE xray_xir_scalar)
xr_enable_pure_aot_symbol_map(test_xr_program_aot_text_output)
add_executable(test_xir_rune xir/test_xir_rune.c)
target_link_libraries(test_xir_rune PRIVATE xray_xir_vm xray_xir_cgen)
set(XIR_RUNE_LEAF_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_rune_leaf.c)
add_custom_command(OUTPUT ${XIR_RUNE_LEAF_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_rune> ${XIR_RUNE_LEAF_GENERATED}
    DEPENDS test_xir_rune VERBATIM)
add_executable(test_xir_rune_native xir/test_xir_rune_native.c ${XIR_RUNE_LEAF_GENERATED})
target_link_libraries(test_xir_rune_native PRIVATE xray_xir_scalar)
xr_enable_pure_aot_symbol_map(test_xir_rune_native)
foreach(rune_target IN ITEMS test_xir_text_output_source test_xr_program_aot_text_output test_xir_rune test_xir_rune_native)
    target_include_directories(${rune_target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/xir)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
        (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
        target_compile_options(${rune_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${rune_target} PRIVATE -std=c11 -pedantic-errors -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_text_output_source COMMAND $<TARGET_FILE:test_xir_text_output_source>)
add_test(NAME test_xir_rune COMMAND $<TARGET_FILE:test_xir_rune>)
set_tests_properties(test_xir_text_output_source test_xir_rune PROPERTIES
    LABELS "unit;xir;canonical-program;execution;string;rune;ownership;abi" TIMEOUT 300)
add_test(NAME test_xr_program_aot_text_output
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xr_program_aot_text_output> --expected-exit 0
        --expected-stdout-hex 2d352074727565206162636420f09f9880207472756520616263640a616263640a0a)
set_tests_properties(test_xr_program_aot_text_output PROPERTIES
    LABELS "unit;aot;canonical-program;generated-c;native;provider;string;pure-aot;task-309")
add_test(NAME test_xir_rune_native
    COMMAND ${XRAY_PYTHON} ${CMAKE_SOURCE_DIR}/scripts/check_xr_program_aot_native.py
        --executable $<TARGET_FILE:test_xir_rune_native> --expected-exit 0 --expected-stdout-hex "")
set_tests_properties(test_xir_rune_native PROPERTIES LABELS "unit;xir;rune;native;generated-c;pure-aot;ownership;abi" TIMEOUT 300)
add_test(NAME test_xir_rune_checked_golden
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_rune_checked_golden.py)
set_tests_properties(test_xir_rune_checked_golden PROPERTIES LABELS "unit;xir;rune;checked;independent-golden" TIMEOUT 30)
