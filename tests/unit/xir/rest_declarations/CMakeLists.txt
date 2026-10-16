add_executable(test_xir_rest_declarations_source test_source.c)
add_executable(test_xir_rest_declarations_wire test_wire.c)
target_link_libraries(test_xir_rest_declarations_wire PRIVATE xray_xir_vm)
target_link_libraries(test_xir_rest_declarations_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(rest_c "${CMAKE_CURRENT_BINARY_DIR}/rest_declarations_generated.c")
add_custom_command(OUTPUT "${rest_c}" "${rest_c}.chk"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/run_source.py"
        $<TARGET_FILE:test_xir_rest_declarations_source> --emit "${rest_c}"
    DEPENDS test_xir_rest_declarations_source run_source.py rest.xr root.xr wrong.xr write.xr ref.xr default.xr fixed_value.xr missing.xr array_argument.xr not_last.xr interface_call.xr interface_value.xr
    VERBATIM)
add_executable(test_xir_rest_declarations_native test_native.c "${rest_c}")
target_link_libraries(test_xir_rest_declarations_native PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_rest_declarations_native PRIVATE REST_PACKET="${rest_c}.chk")
foreach(target IN ITEMS test_xir_rest_declarations_source test_xir_rest_declarations_native test_xir_rest_declarations_wire)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_rest_declarations_source
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_rest_declarations_source>)
set_tests_properties(test_xir_rest_declarations_source PROPERTIES TIMEOUT 150 LABELS "unit;xir;library;ownership;rest")
add_test(NAME test_xir_rest_declarations_native COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_rest_declarations_native> --direct)
set_tests_properties(test_xir_rest_declarations_native PROPERTIES TIMEOUT 30 LABELS "unit;xir;library;ownership;rest;native;mixed")
add_test(NAME test_xir_rest_declarations_wire COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_rest_declarations_wire> --direct)
set_tests_properties(test_xir_rest_declarations_wire PROPERTIES TIMEOUT 120 LABELS "unit;xir;wire;ownership;rest;budget")
add_test(NAME test_xir_rest_declarations_wire_fields COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/derive_wire.py")
set_tests_properties(test_xir_rest_declarations_wire_fields PROPERTIES TIMEOUT 30 LABELS "unit;xir;wire;oracle;rest")
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
