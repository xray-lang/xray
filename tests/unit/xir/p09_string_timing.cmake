set(P09_TIMING_C "${CMAKE_BINARY_DIR}/generated/xir_p09_string_timing.c")
add_executable(test_xir_p09_string_timing "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_timing.c")
target_link_libraries(test_xir_p09_string_timing PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${P09_TIMING_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_string_timing> --generate "${P09_TIMING_C}"
    DEPENDS test_xir_p09_string_timing
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/string_build/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/string_build/producer.xr" VERBATIM)
add_executable(test_xir_p09_string_timing_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_timing.c" "${P09_TIMING_C}")
target_link_libraries(test_xir_p09_string_timing_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_p09_string_timing_native PRIVATE XR_STRING_TIMING_NATIVE=1)
add_executable(test_xir_p09_builder_timing "${PROJECT_SOURCE_DIR}/tests/unit/xir/p09_builder_baseline/test_xir_builder_timing.c")
target_include_directories(test_xir_p09_builder_timing PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
foreach(target test_xir_p09_string_timing test_xir_p09_string_timing_native test_xir_p09_builder_timing)
    target_compile_definitions(${target} PRIVATE XR_TIMING_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(scenario short digits chunks join snapshot)
    add_test(NAME test_xir_p09_timing_vm_${scenario} COMMAND test_xir_p09_string_timing 0 ${scenario})
    add_test(NAME test_xir_p09_timing_native_${scenario} COMMAND test_xir_p09_string_timing_native 1 ${scenario})
    add_test(NAME test_xir_p09_timing_builder_${scenario} COMMAND test_xir_p09_builder_timing ${scenario})
    set_tests_properties(test_xir_p09_timing_vm_${scenario} test_xir_p09_timing_native_${scenario}
        test_xir_p09_timing_builder_${scenario} PROPERTIES TIMEOUT 120 LABELS "measurement;string;content;timing")
endforeach()
