# One complete program and its actual generated native translation unit.
set(REMOVE_C "${CMAKE_BINARY_DIR}/generated/xir_array_remove.c")
add_executable(test_xir_array_remove "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_remove.c")
target_link_libraries(test_xir_array_remove PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${REMOVE_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_remove> 0 "${REMOVE_C}"
    DEPENDS test_xir_array_remove "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/transaction/root.xr"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/transaction/producer.xr" VERBATIM)
add_executable(test_xir_array_remove_native "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_remove.c" "${REMOVE_C}")
target_link_libraries(test_xir_array_remove_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_remove_native PRIVATE XR_REMOVE_NATIVE=1)
foreach(target test_xir_array_remove test_xir_array_remove_native)
    target_compile_definitions(${target} PRIVATE XR_REMOVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_remove_vm COMMAND test_xir_array_remove 0)
add_test(NAME test_xir_array_remove_native COMMAND test_xir_array_remove_native 1)
add_test(NAME test_xir_array_remove_mixed_root COMMAND test_xir_array_remove_native 2)
add_test(NAME test_xir_array_remove_mixed_import COMMAND test_xir_array_remove_native 3)
set_tests_properties(test_xir_array_remove_vm test_xir_array_remove_native test_xir_array_remove_mixed_root
    test_xir_array_remove_mixed_import PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution")

set(remove_modes vm native mixed_root mixed_import)
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach(mode RANGE 0 3)
        list(GET remove_modes ${mode} suffix)
        if(mode EQUAL 0)
            set(binary test_xir_array_remove)
        else()
            set(binary test_xir_array_remove_native)
        endif()
        add_test(NAME test_xir_array_remove_compiler_${suffix}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/unit/xir/run_array_remove_compiler.py"
                --executable $<TARGET_FILE:${binary}> --mode ${mode} --workers 8
                --reports-root "${CMAKE_BINARY_DIR}/array-remove-compiler-reports/${suffix}")
    endforeach()
else()
    add_test(NAME test_xir_array_remove_compiler_vm COMMAND test_xir_array_remove --compiler 0)
    add_test(NAME test_xir_array_remove_compiler_native COMMAND test_xir_array_remove_native --compiler 1)
    add_test(NAME test_xir_array_remove_compiler_mixed_root COMMAND test_xir_array_remove_native --compiler 2)
    add_test(NAME test_xir_array_remove_compiler_mixed_import COMMAND test_xir_array_remove_native --compiler 3)
endif()
set_tests_properties(test_xir_array_remove_compiler_vm test_xir_array_remove_compiler_native
    test_xir_array_remove_compiler_mixed_root test_xir_array_remove_compiler_mixed_import
    PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;xir;array;memory;ownership;budget")
if(WIN32)
    set_tests_properties(test_xir_array_remove_compiler_vm test_xir_array_remove_compiler_native
        test_xir_array_remove_compiler_mixed_root test_xir_array_remove_compiler_mixed_import PROPERTIES PROCESSORS 8)
endif()

foreach(group RANGE 1 7)
    set(generated "${CMAKE_BINARY_DIR}/generated/xir_array_remove_matrix${group}.c")
    set(vm "test_xir_array_remove_matrix${group}")
    set(native "${vm}_native")
    add_executable(${vm} "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_remove.c")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:${vm}> 0 "${generated}"
        DEPENDS ${vm} "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/matrix${group}/root.xr"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove/matrix${group}/producer.xr" VERBATIM)
    add_executable(${native} "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_remove.c" "${generated}")
    target_compile_definitions(${native} PRIVATE XR_REMOVE_NATIVE=1)
    foreach(target ${vm} ${native})
        target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
        target_compile_definitions(${target} PRIVATE XR_REMOVE_MATRIX=${group} XR_REMOVE_COMPONENT="matrix${group}"
            XR_REMOVE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_remove")
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    add_test(NAME ${vm}_vm COMMAND ${vm} 0)
    add_test(NAME ${vm}_native COMMAND ${native} 1)
    add_test(NAME ${vm}_mixed_root COMMAND ${native} 2)
    add_test(NAME ${vm}_mixed_import COMMAND ${native} 3)
    set_tests_properties(${vm}_vm ${vm}_native ${vm}_mixed_root ${vm}_mixed_import
        PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;memory;ownership;execution")
    if(group EQUAL 7)
        foreach(mode RANGE 0 3)
            list(GET remove_modes ${mode} suffix)
            if(mode EQUAL 0)
                set(binary ${vm})
            else()
                set(binary ${native})
            endif()
            if(WIN32)
                add_test(NAME ${vm}_compiler_${suffix}
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/unit/xir/run_array_remove_compiler.py"
                        --executable $<TARGET_FILE:${binary}> --mode ${mode} --workers 8 --component matrix7
                        --reports-root "${CMAKE_BINARY_DIR}/array-remove-compiler-reports/class_${suffix}")
            else()
                add_test(NAME ${vm}_compiler_${suffix} COMMAND ${binary} --compiler ${mode})
            endif()
            set_tests_properties(${vm}_compiler_${suffix} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
                LABELS "unit;xir;array;memory;ownership;budget")
            if(WIN32)
                set_tests_properties(${vm}_compiler_${suffix} PROPERTIES PROCESSORS 8)
            endif()
        endforeach()
    endif()
endforeach()

add_test(NAME test_xir_array_remove_rejections COMMAND test_xir_array_remove --rejections)
set_tests_properties(test_xir_array_remove_rejections PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;permissions")
