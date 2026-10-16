add_executable(test_xir_export_constants_source test_source.c)
add_executable(test_xir_export_constants_wire test_wire.c)
target_link_libraries(test_xir_export_constants_wire PRIVATE xray_xir_vm)
target_link_libraries(test_xir_export_constants_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(constants_c "${CMAKE_CURRENT_BINARY_DIR}/export_constants_generated.c")
add_custom_command(OUTPUT "${constants_c}" "${constants_c}.chk"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/run_source.py"
        $<TARGET_FILE:test_xir_export_constants_source> --emit "${constants_c}"
    DEPENDS test_xir_export_constants_source run_source.py constants.xr root.xr private.xr write.xr mutable.xr
    VERBATIM)
add_executable(test_xir_export_constants_native test_native.c "${constants_c}")
target_link_libraries(test_xir_export_constants_native PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_export_constants_native PRIVATE CONSTANT_PACKET="${constants_c}.chk")
foreach(target IN ITEMS test_xir_export_constants_source test_xir_export_constants_native test_xir_export_constants_wire)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_export_constants_source
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_export_constants_source>)
set_tests_properties(test_xir_export_constants_source PROPERTIES TIMEOUT 150 LABELS "unit;xir;library;ownership;constants")
add_test(NAME test_xir_export_constants_native COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_export_constants_native> --direct)
set_tests_properties(test_xir_export_constants_native PROPERTIES TIMEOUT 30 LABELS "unit;xir;library;ownership;constants;native;mixed")
add_test(NAME test_xir_export_constants_wire COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_export_constants_wire> --direct)
set_tests_properties(test_xir_export_constants_wire PROPERTIES TIMEOUT 120 LABELS "unit;xir;wire;ownership;constants;budget")
add_test(NAME test_xir_export_constants_wire_fields COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/derive_wire.py")
set_tests_properties(test_xir_export_constants_wire_fields PROPERTIES TIMEOUT 30 LABELS "unit;xir;wire;oracle;constants")
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
