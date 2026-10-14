set(P09_STRING_BUILD_C "${CMAKE_BINARY_DIR}/generated/xir_p09_string_build.c")
add_executable(test_xir_p09_string_build "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_build.c")
target_link_libraries(test_xir_p09_string_build PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${P09_STRING_BUILD_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_string_build> --generate "${P09_STRING_BUILD_C}"
    DEPENDS test_xir_p09_string_build
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/string_build/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/string_build/producer.xr" VERBATIM)
add_executable(test_xir_p09_string_build_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_string_build.c" "${P09_STRING_BUILD_C}")
target_link_libraries(test_xir_p09_string_build_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_p09_string_build_native PRIVATE XR_CAPACITY_NATIVE=1)
foreach(target test_xir_p09_string_build test_xir_p09_string_build_native)
    target_compile_definitions(${target} PRIVATE XR_CAPACITY_COMPONENT="string_build"
        XR_CAPACITY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(ENABLE_ASAN AND WIN32 AND (MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
        target_link_options(${target} PRIVATE "LINKER:/STACK:16777216")
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(target test_xir_p09_string_build)
    else()
        set(target test_xir_p09_string_build_native)
    endif()
    foreach(scenario short digits chunks join snapshot scalars)
        add_test(NAME test_xir_p09_string_build_${scenario}_${mode} COMMAND ${target} ${mode} ${scenario})
        set_tests_properties(test_xir_p09_string_build_${scenario}_${mode} PROPERTIES TIMEOUT 120
            LABELS "unit;xir;string;ownership;execution;measurement")
    endforeach()
endforeach()
