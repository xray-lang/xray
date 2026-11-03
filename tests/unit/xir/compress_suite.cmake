add_executable(test_xir_compress_suite "${CMAKE_CURRENT_LIST_DIR}/test_xir_compress_suite.c")
target_link_libraries(test_xir_compress_suite PRIVATE xray_xir_source_product)
set(compress_suite_dir "${CMAKE_CURRENT_BINARY_DIR}/compress-suite")
set(compress_suite_c "${compress_suite_dir}/catalog.c")
add_custom_command(OUTPUT "${compress_suite_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${compress_suite_dir}"
    COMMAND $<TARGET_FILE:test_xir_compress_suite> "${PROJECT_SOURCE_DIR}"
        "${compress_suite_c}" "${compress_suite_dir}/generation.json" --emit-only
    DEPENDS test_xir_compress_suite
        "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/1301_compress.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/compress/compress.xr"
        "${CMAKE_CURRENT_LIST_DIR}/compress_suite_inventory.h"
        "${CMAKE_CURRENT_LIST_DIR}/compress_suite_runtime.h"
    VERBATIM)
add_executable(test_xir_compress_suite_native
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_compress_suite_native.c" "${compress_suite_c}")
target_link_libraries(test_xir_compress_suite_native PRIVATE xray_xir_scalar)
foreach(target test_xir_compress_suite test_xir_compress_suite_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME xir_compress_suite
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_LIST_DIR}/run_compress_suite.py"
        --root "${PROJECT_SOURCE_DIR}" --vm $<TARGET_FILE:test_xir_compress_suite>
        --native $<TARGET_FILE:test_xir_compress_suite_native>
        --generated-c "${compress_suite_c}" --reports "${compress_suite_dir}/reports")
add_test(NAME cli_compress_consumers
    COMMAND ${XRAY_PYTHON} -X utf8 "${PROJECT_SOURCE_DIR}/tests/cli/run_language_surface_tests.py"
        --xray $<TARGET_FILE:xray>
        --case "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_checksum_direct.xr"
            "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_checksum_direct.xr.expected"
        --case "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr"
            "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_roundtrip_direct.xr.expected"
        --case "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_truncated_direct.xr"
            "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_truncated_direct.xr.expected"
        --case "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_external_codec_direct.xr"
            "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/stdlib/compress_external_codec_direct.xr.expected")
set_tests_properties(xir_compress_suite cli_compress_consumers PROPERTIES
    TIMEOUT 600 LABELS "unit;xir;source;stdlib;compress;ownership;generated-c")
