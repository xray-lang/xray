add_executable(test_xir_source_copy xir/test_xir_source_copy.c)
target_link_libraries(test_xir_source_copy PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(copy_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_copy.c")
add_custom_command(OUTPUT "${copy_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_source_copy> "${copy_generated}"
    DEPENDS test_xir_source_copy VERBATIM)
add_executable(test_xir_source_copy_native xir/test_xir_source_copy_native.c "${copy_generated}")
target_link_libraries(test_xir_source_copy_native PRIVATE xray_xir_scalar)
foreach(target test_xir_source_copy test_xir_source_copy_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_source_copy COMMAND test_xir_source_copy)
add_test(NAME xir_source_copy_native COMMAND test_xir_source_copy_native)
set_tests_properties(xir_source_copy xir_source_copy_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;copy;ownership;generated-c")
if(WIN32 AND TARGET xray)
    add_test(NAME xir_source_copy_readmes COMMAND ${XRAY_PYTHON} -X utf8
        "${PROJECT_SOURCE_DIR}/tests/cli/test_readme_copy.py" --root "${PROJECT_SOURCE_DIR}"
        --binary $<TARGET_FILE:xray> --output "${CMAKE_BINARY_DIR}/readme-copy-clients")
    set_tests_properties(xir_source_copy_readmes PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;source;readme;copy;native")
endif()
