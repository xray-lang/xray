set(go_closure_c "${CMAKE_CURRENT_BINARY_DIR}/generated/go_closure.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(test_xir_go_closure xir/test_xir_go_closure.c)
target_link_libraries(test_xir_go_closure PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${go_closure_c}"
    COMMAND $<TARGET_FILE:test_xir_go_closure> --emit "${go_closure_c}"
    DEPENDS test_xir_go_closure "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_closure/closure.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_closure/producer.xr" VERBATIM)
add_executable(test_xir_go_closure_native xir/test_xir_go_closure.c "${go_closure_c}")
target_compile_definitions(test_xir_go_closure_native PRIVATE XR_GO_CLOSURE_NATIVE=1)
target_link_libraries(test_xir_go_closure_native PRIVATE xray_xir_scalar)
foreach(target test_xir_go_closure test_xir_go_closure_native)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE
        XR_GO_CLOSURE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_closure"
        XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME xir_go_closure COMMAND test_xir_go_closure)
add_test(NAME xir_go_closure_native COMMAND test_xir_go_closure_native)
set_tests_properties(xir_go_closure xir_go_closure_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;task;source;ownership")
