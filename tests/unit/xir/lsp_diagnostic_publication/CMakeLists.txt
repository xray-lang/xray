if(NOT WIN32 OR TARGET test_lsp_diagnostic_publication)
 return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test_lsp_diagnostic_publication "${CMAKE_CURRENT_LIST_DIR}/test_publication.c" "${CMAKE_CURRENT_LIST_DIR}/transport_owner.c"
 "${PROJECT_SOURCE_DIR}/tests/unit/xir/lsp_document_owner/owner.c"
 "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_diagnostic_publish.c" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_diagnostic_clear.c"
 "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_protocol_message.c" "${PROJECT_SOURCE_DIR}/src/app/lsp/xlsp_framing.c"
 "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c" "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c")
target_link_libraries(test_lsp_diagnostic_publication PRIVATE xray_lsp_document_open bcrypt)
target_include_directories(test_lsp_diagnostic_publication PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base" "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/tests/unit/xir")
target_compile_features(test_lsp_diagnostic_publication PRIVATE c_std_11)
if(MSVC)
 target_compile_options(test_lsp_diagnostic_publication PRIVATE /W4 /WX /utf-8)
else()
 target_compile_options(test_lsp_diagnostic_publication PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode normal resources)
 add_test(NAME test_lsp_diagnostic_publication_${mode} COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_publication.py" "$<TARGET_FILE:test_lsp_diagnostic_publication>" "${PROJECT_SOURCE_DIR}/stdlib" --${mode})
 set_tests_properties(test_lsp_diagnostic_publication_${mode} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;lsp;source;protocol;ownership;allocation")
endforeach()
set_tests_properties(test_lsp_diagnostic_publication_resources PROPERTIES TIMEOUT 300 PROCESSORS 8 COST 290)
