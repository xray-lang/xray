if(TARGET test_lsp_envelope)
    return()
endif()
foreach(target test_lsp_envelope test_lsp_envelope_before)
    if(target STREQUAL "test_lsp_envelope")
        set(entry test_envelope.c)
    else()
        set(entry test_before.c)
    endif()
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/${entry}" "${PROJECT_SOURCE_DIR}/src/base/xutf8.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/src/app/lsp" "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_lsp_envelope_normal COMMAND test_lsp_envelope)
add_test(NAME test_lsp_envelope_resources COMMAND test_lsp_envelope --resources)
add_test(NAME test_lsp_envelope_before COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_before.py" "$<TARGET_FILE:test_lsp_envelope_before>")
set_tests_properties(test_lsp_envelope_normal test_lsp_envelope_resources test_lsp_envelope_before PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;protocol;ownership;allocation")

set(dispatch_dir "${CMAKE_CURRENT_BINARY_DIR}/lsp-envelope-dispatch")
add_custom_command(OUTPUT "${dispatch_dir}/envelope_dispatch.inc.c"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_dispatch.py"
        "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${dispatch_dir}/envelope_dispatch.inc.c"
    DEPENDS "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_server.c" "${CMAKE_CURRENT_LIST_DIR}/extract_dispatch.py")
add_custom_command(OUTPUT "${dispatch_dir}/before_dispatch.inc.c"
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/extract_dispatch.py"
        "${CMAKE_CURRENT_LIST_DIR}/before_server.c" "${dispatch_dir}/before_dispatch.inc.c" --predecessor
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/before_server.c" "${CMAKE_CURRENT_LIST_DIR}/extract_dispatch.py")
set_source_files_properties("${dispatch_dir}/envelope_dispatch.inc.c" "${dispatch_dir}/before_dispatch.inc.c" PROPERTIES HEADER_FILE_ONLY TRUE)
add_executable(test_lsp_envelope_dispatch "${CMAKE_CURRENT_LIST_DIR}/test_dispatch.c"
    "${PROJECT_SOURCE_DIR}/src/base/xutf8.c" "${dispatch_dir}/envelope_dispatch.inc.c" "${dispatch_dir}/before_dispatch.inc.c")
target_include_directories(test_lsp_envelope_dispatch PRIVATE "${dispatch_dir}" "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/src/app/lsp" "${PROJECT_SOURCE_DIR}/include")
target_compile_features(test_lsp_envelope_dispatch PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_lsp_envelope_dispatch PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_lsp_envelope_dispatch PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_lsp_envelope_dispatch COMMAND test_lsp_envelope_dispatch)
set_tests_properties(test_lsp_envelope_dispatch PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;protocol;ownership")

add_test(NAME test_lsp_envelope_progress_before COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_before.py" "$<TARGET_FILE:test_lsp_envelope_dispatch>" --progress)
set_tests_properties(test_lsp_envelope_progress_before PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE LABELS "unit;lsp;protocol;ownership")
