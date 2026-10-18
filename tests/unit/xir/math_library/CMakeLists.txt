add_executable(test_xir_math_core test_math_core.c)
add_executable(test_xir_math_wire test_wire.c)
target_link_libraries(test_xir_math_wire PRIVATE xray_xir_vm)
add_executable(test_xir_math_source_faults test_source_faults.c)
target_link_libraries(test_xir_math_source_faults PRIVATE xray_xir_source)
target_link_libraries(test_xir_math_core PRIVATE xray_xir_scalar)
set_target_properties(test_xir_math_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_math_core PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_math_core PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_math_core COMMAND test_xir_math_core)
set_tests_properties(test_xir_math_core PROPERTIES TIMEOUT 30 LABELS "unit;xir;math;numeric;random")
add_test(NAME test_xir_math_semantic79_fields COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/../derive_semantic79_vectors.py")
set_tests_properties(test_xir_math_semantic79_fields PROPERTIES TIMEOUT 30 LABELS "unit;xir;math;wire;oracle")
add_executable(test_xir_math_library_source test_source.c)
target_link_libraries(test_xir_math_library_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(math_c "${CMAKE_CURRENT_BINARY_DIR}/math_library_generated.c")
add_custom_command(OUTPUT "${math_c}" "${math_c}.chk"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/run_source.py"
        $<TARGET_FILE:test_xir_math_library_source> --emit "${math_c}"
    DEPENDS test_xir_math_library_source run_source.py root.xr ctfe.xr ${PROJECT_SOURCE_DIR}/stdlib/math/math.xr
    VERBATIM)
add_executable(test_xir_math_library_native test_native.c "${math_c}")
target_link_libraries(test_xir_math_library_native PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_math_library_native PRIVATE MATH_PACKET="${math_c}.chk")
foreach(target IN ITEMS test_xir_math_library_source test_xir_math_library_native test_xir_math_wire test_xir_math_source_faults)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_math_library_source COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_math_library_source>)
set_tests_properties(test_xir_math_library_source PROPERTIES TIMEOUT 150 LABELS "unit;xir;library;math;ownership;ctfe")
add_test(NAME test_xir_math_library_native COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_math_library_native> --direct)
set_tests_properties(test_xir_math_library_native PROPERTIES TIMEOUT 30 LABELS "unit;xir;library;math;ownership;native;mixed")
add_test(NAME test_xir_math_wire COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source.py" $<TARGET_FILE:test_xir_math_wire> --direct)
set_tests_properties(test_xir_math_wire PROPERTIES TIMEOUT 120 LABELS "unit;xir;math;wire;budget;ownership")
add_test(NAME test_xir_math_wire_fields COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/derive_wire.py")
set_tests_properties(test_xir_math_wire_fields PROPERTIES TIMEOUT 30 LABELS "unit;xir;math;wire;oracle")
add_test(NAME test_xir_math_source_faults COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source_faults.py" $<TARGET_FILE:test_xir_math_source_faults>)
set_tests_properties(test_xir_math_source_faults PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;xir;math;source;budget;ownership")
add_test(NAME test_xir_math_public_cli COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/cli/run_math_checked_catalog_tests.py" $<TARGET_FILE:xray>)
set_tests_properties(test_xir_math_public_cli PROPERTIES TIMEOUT 120 LABELS "unit;xir;math;cli;catalog;ctfe;ownership")
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
