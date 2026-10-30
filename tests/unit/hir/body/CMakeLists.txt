add_executable(test_hir_body test_body.c)
target_link_libraries(test_hir_body PRIVATE xray_xir_source)
target_include_directories(test_hir_body PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_executable(test_hir_body_source test_body_source.c)
if(WIN32)
    target_link_libraries(test_hir_body_source PRIVATE ${XRAY_SOURCE_SDK_ARCHIVE_TARGETS})
else()
    target_link_libraries(test_hir_body_source PRIVATE xray_source_api)
endif()
target_include_directories(test_hir_body_source PRIVATE "${PROJECT_SOURCE_DIR}/include")
if(MSVC)
    set_target_properties(test_hir_body_source PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()
foreach(target test_hir_body test_hir_body_source)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib")
add_test(NAME hir_body COMMAND test_hir_body)
set_tests_properties(hir_body PROPERTIES FIXTURES_SETUP hir_body_inputs WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME source_body_sdk COMMAND test_hir_body_source "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib"
    "${CMAKE_CURRENT_BINARY_DIR}/control.xr" "${CMAKE_CURRENT_BINARY_DIR}/control.out"
    "${PROJECT_SOURCE_DIR}/tests/diff/cases/semantics/string_owning_parameter.xr" "${CMAKE_CURRENT_BINARY_DIR}/original.out"
    "${CMAKE_CURRENT_BINARY_DIR}/uses.xr" "${CMAKE_CURRENT_BINARY_DIR}/uses.out"
    "${CMAKE_CURRENT_BINARY_DIR}/places.xr" "${CMAKE_CURRENT_BINARY_DIR}/places.out"
    "${CMAKE_CURRENT_BINARY_DIR}/callback.xr" "${CMAKE_CURRENT_BINARY_DIR}/callback.out"
    "${CMAKE_CURRENT_BINARY_DIR}/callback-family.xr" "${CMAKE_CURRENT_BINARY_DIR}/callback-family.out")
set_tests_properties(source_body_sdk PROPERTIES FIXTURES_REQUIRED hir_body_inputs)
set_tests_properties(hir_body source_body_sdk PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;ownership;sdk")
if(WIN32)
    add_test(NAME hir_body_cli COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/test_body_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --client $<TARGET_FILE:test_hir_body_source> --pipeline $<TARGET_FILE:lir_pipeline_eval>)
    set_tests_properties(hir_body_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "unit;hir;types;cli;native;ownership")
endif()
