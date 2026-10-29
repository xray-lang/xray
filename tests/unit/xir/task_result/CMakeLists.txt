get_directory_property(task_result_compile_options COMPILE_OPTIONS)
list(REMOVE_ITEM task_result_compile_options /std:c11 /experimental:c11atomics)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${task_result_compile_options}")
set(task_result_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/task_result.xr")
set(task_result_oracle "${CMAKE_CURRENT_LIST_DIR}/fixtures/task_result.expected.json")
set(task_result_entries successResult errorResult panicResult cancelledResult)
add_executable(test_xir_task_result_source "${CMAKE_CURRENT_LIST_DIR}/test_task_result_source.c")
target_link_libraries(test_xir_task_result_source PRIVATE xray_source_api)
add_test(NAME test_xir_task_result_source COMMAND test_xir_task_result_source
    "${PROJECT_SOURCE_DIR}/stdlib" "${task_result_fixture}")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/task_result.c")
add_custom_command(OUTPUT "${generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${task_result_fixture}" "${generated}"
    DEPENDS test_xir_from_utf8 "${task_result_fixture}" VERBATIM)
add_executable(test_xir_task_result_native "${CMAKE_CURRENT_LIST_DIR}/test_task_result_native.c" "${generated}")
add_executable(test_xir_task_result_runtime "${CMAKE_CURRENT_LIST_DIR}/test_task_result_runtime.c" "${generated}")
add_executable(test_xir_task_result_cpp "${CMAKE_CURRENT_LIST_DIR}/test_task_result_cpp.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/test_task_result_native.c" "${generated}")
target_compile_definitions(test_xir_task_result_cpp PRIVATE XR_TASK_RESULT_BRIDGE=1)
foreach(target test_xir_task_result_native test_xir_task_result_runtime test_xir_task_result_cpp)
    target_link_libraries(${target} PRIVATE xray_xir_scalar xray_compile_resources)
endforeach()
foreach(target test_xir_task_result_source test_xir_task_result_native test_xir_task_result_runtime test_xir_task_result_cpp)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /bigobj)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME test_xir_task_result_native COMMAND test_xir_task_result_native ${task_result_entries} "${task_result_oracle}")
add_test(NAME test_xir_task_result_cpp COMMAND test_xir_task_result_cpp ${task_result_entries} "${task_result_oracle}")
add_test(NAME test_xir_task_result_runtime COMMAND test_xir_task_result_runtime)
set_tests_properties(test_xir_task_result_source test_xir_task_result_native test_xir_task_result_cpp test_xir_task_result_runtime
    PROPERTIES LABELS "unit;xir;language;ownership;task" TIMEOUT 240)
add_test(NAME test_xir_task_result_cli
    COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_LIST_DIR}/test_task_result_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --source "${task_result_fixture}" --expect "${task_result_oracle}")
set_tests_properties(test_xir_task_result_cli PROPERTIES
    LABELS "unit;xir;language;ownership;task;cli" TIMEOUT 240)
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
