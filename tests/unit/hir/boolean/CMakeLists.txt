add_executable(test_hir_boolean test_boolean.c)
target_link_libraries(test_hir_boolean PRIVATE xray_xir_source)




foreach(target test_hir_boolean )
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME hir_boolean COMMAND test_hir_boolean)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib")

set_tests_properties(hir_boolean  PROPERTIES TIMEOUT 60 LABELS "unit;hir;types;ownership;sdk")
if(WIN32)
    add_test(NAME hir_boolean_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/test_boolean_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>)
    set_tests_properties(hir_boolean_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
        LABELS "unit;hir;types;cli;native")
endif()
