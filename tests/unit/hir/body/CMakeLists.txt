add_executable(test_hir_body test_body.c)
target_link_libraries(test_hir_body PRIVATE xray_xir_source)
target_include_directories(test_hir_body PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(test_hir_tuple_control test_tuple_source_control.c)
add_executable(test_hir_nominal_control test_nominal_source_control.c)
target_link_libraries(test_hir_nominal_control PRIVATE xray_xir_source)
target_include_directories(test_hir_nominal_control PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_hir_tuple_control PRIVATE xray_xir_source)
target_include_directories(test_hir_tuple_control PRIVATE "${PROJECT_SOURCE_DIR}/src")



foreach(target test_hir_body  test_hir_tuple_control test_hir_nominal_control)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-stdlib")
add_test(NAME hir_body COMMAND test_hir_body)
add_test(NAME hir_tuple_source_control COMMAND test_hir_tuple_control "${CMAKE_CURRENT_SOURCE_DIR}/tuple_contracts.xr")
set_tests_properties(hir_tuple_source_control PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;source;ctfe;ownership;resources")
add_test(NAME hir_nominal_source_control COMMAND test_hir_nominal_control "${CMAKE_CURRENT_SOURCE_DIR}/nominal_contracts.xr")
set_tests_properties(hir_nominal_source_control PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;source;ctfe;ownership;resources")
set_tests_properties(hir_body PROPERTIES FIXTURES_SETUP hir_body_inputs WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")







set_tests_properties(hir_body
    PROPERTIES TIMEOUT 120 LABELS "unit;hir;types;ownership;sdk")
