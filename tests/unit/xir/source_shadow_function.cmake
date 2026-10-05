set(SHADOW_C "${CMAKE_BINARY_DIR}/generated/xir_shadow_function.c")
add_executable(test_xir_source_shadow_function "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_source_shadow_function.c")
add_custom_command(OUTPUT "${SHADOW_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_source_shadow_function> 0 "${SHADOW_C}"
    DEPENDS test_xir_source_shadow_function "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/shadow_function/root.xr"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/shadow_function/producer.xr" VERBATIM)
add_executable(test_xir_source_shadow_function_native "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_source_shadow_function.c" "${SHADOW_C}")
target_compile_definitions(test_xir_source_shadow_function_native PRIVATE XR_REMOVE_NATIVE=1)
foreach(target test_xir_source_shadow_function test_xir_source_shadow_function_native)
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
    target_compile_definitions(${target} PRIVATE XR_REMOVE_COMPONENT="shadow_function"
        XR_REMOVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_source_shadow_function_vm COMMAND test_xir_source_shadow_function 0)
add_test(NAME test_xir_source_shadow_function_native COMMAND test_xir_source_shadow_function_native 1)
add_test(NAME test_xir_source_shadow_function_mixed_root COMMAND test_xir_source_shadow_function_native 2)
add_test(NAME test_xir_source_shadow_function_mixed_import COMMAND test_xir_source_shadow_function_native 3)
set_tests_properties(test_xir_source_shadow_function_vm test_xir_source_shadow_function_native
    test_xir_source_shadow_function_mixed_root test_xir_source_shadow_function_mixed_import
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;generic;ownership")

set(SHADOW_REF_C "${CMAKE_BINARY_DIR}/generated/xir_shadow_ref.c")
add_executable(test_xir_source_shadow_ref "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_source_shadow_function.c")
add_custom_command(OUTPUT "${SHADOW_REF_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_source_shadow_ref> 0 "${SHADOW_REF_C}"
    DEPENDS test_xir_source_shadow_ref "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/shadow_ref/root.xr"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/shadow_ref/producer.xr" VERBATIM)
add_executable(test_xir_source_shadow_ref_native "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_source_shadow_function.c" "${SHADOW_REF_C}")
target_compile_definitions(test_xir_source_shadow_ref_native PRIVATE XR_REMOVE_NATIVE=1)
foreach(target test_xir_source_shadow_ref test_xir_source_shadow_ref_native)
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
    target_compile_definitions(${target} PRIVATE XR_SHADOW_REF=1 XR_REMOVE_COMPONENT="shadow_ref"
        XR_REMOVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_source_shadow_ref_vm COMMAND test_xir_source_shadow_ref 0)
add_test(NAME test_xir_source_shadow_ref_native COMMAND test_xir_source_shadow_ref_native 1)
add_test(NAME test_xir_source_shadow_ref_mixed_root COMMAND test_xir_source_shadow_ref_native 2)
add_test(NAME test_xir_source_shadow_ref_mixed_import COMMAND test_xir_source_shadow_ref_native 3)
set_tests_properties(test_xir_source_shadow_ref_vm test_xir_source_shadow_ref_native
    test_xir_source_shadow_ref_mixed_root test_xir_source_shadow_ref_mixed_import
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;generic;ownership")

add_test(NAME test_xir_source_shadow_function_rejections COMMAND test_xir_source_shadow_function --rejections)
set_tests_properties(test_xir_source_shadow_function_rejections PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;generic;permissions")

set(shadow_modes vm native mixed_root mixed_import)
foreach(component shadow_function shadow_ref)
    foreach(mode RANGE 0 3)
        list(GET shadow_modes ${mode} suffix)
        if(mode EQUAL 0)
            set(binary test_xir_source_${component})
        else()
            set(binary test_xir_source_${component}_native)
        endif()
        if(WIN32)
            find_package(Python3 COMPONENTS Interpreter REQUIRED)
            add_test(NAME test_xir_source_${component}_compiler_${suffix}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/unit/xir/run_source_shadow_compiler.py"
                    --executable $<TARGET_FILE:${binary}> --mode ${mode} --workers 8 --component ${component}
                    --reports-root "${CMAKE_BINARY_DIR}/source-shadow-compiler-reports/${component}_${suffix}")
            set_tests_properties(test_xir_source_${component}_compiler_${suffix} PROPERTIES PROCESSORS 8)
        else()
            add_test(NAME test_xir_source_${component}_compiler_${suffix} COMMAND ${binary} --compiler ${mode})
        endif()
        set_tests_properties(test_xir_source_${component}_compiler_${suffix} PROPERTIES
            TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;xir;source;generic;memory;budget;ownership")
    endforeach()
endforeach()
