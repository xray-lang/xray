add_executable(test_xir_path_prelude xir/test_xir_path_prelude.c)
target_link_libraries(test_xir_path_prelude PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe)
target_include_directories(test_xir_path_prelude PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(test_xir_path_prelude PRIVATE XR_PATH_ROOT="${PROJECT_SOURCE_DIR}" XR_PATH_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude")
set_target_properties(test_xir_path_prelude PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_path_prelude PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_path_prelude PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_path_prelude PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xir_path_prelude COMMAND test_xir_path_prelude)
set_tests_properties(xir_path_prelude PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership")
set(path_type_c "${CMAKE_CURRENT_BINARY_DIR}/generated/path_type_only.c")
set(path_import_c "${CMAKE_CURRENT_BINARY_DIR}/generated/path_explicit.c")
add_custom_command(OUTPUT "${path_type_c}" "${path_import_c}" "${path_type_c}.catalog.c" "${path_type_c}.complete.c" "${path_type_c}.flavours.c"
    COMMAND $<TARGET_FILE:test_xir_path_prelude> --emit "${path_type_c}" "${path_import_c}"
    DEPENDS test_xir_path_prelude
        "${CMAKE_CURRENT_LIST_DIR}/xir_path_complete_source.h"
        "${CMAKE_CURRENT_LIST_DIR}/xir_path_complete_runtime.h"
        "${CMAKE_CURRENT_LIST_DIR}/xir_path_flavours_source.h"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/flavours.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/previous134-native-path/old134-path.xrchk"
        "${PROJECT_SOURCE_DIR}/stdlib/path/posix.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/windows.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/logical.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/path/_lexical.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/cold-stdlib/path/path.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/custom.xr" VERBATIM)
add_executable(test_xir_path_prelude_native xir/test_xir_path_prelude_native.c "${path_type_c}" "${path_import_c}" "${path_type_c}.catalog.c" "${path_type_c}.complete.c" "${path_type_c}.flavours.c")
target_link_libraries(test_xir_path_prelude_native PRIVATE xray_xir_scalar)
target_include_directories(test_xir_path_prelude_native PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_xir_path_prelude_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_path_prelude_native PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_path_prelude_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_path_prelude_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xir_path_prelude_native COMMAND test_xir_path_prelude_native)
set_tests_properties(xir_path_prelude_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership")
add_test(NAME xir_path_previous123 COMMAND test_xir_path_prelude --previous123 "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_path_prelude/previous123-read-ref.xir")
set_tests_properties(xir_path_previous123 PROPERTIES TIMEOUT 30 LABELS "unit;xir;checked;ownership")
add_test(NAME xir_path_prelude_projection COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/test_path_prelude_projection.py")
set_tests_properties(xir_path_prelude_projection PROPERTIES TIMEOUT 30 LABELS "unit;xir;metadata")
