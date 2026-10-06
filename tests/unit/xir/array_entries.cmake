# Complete entry sequences use the same finite owner in every backend.
set(ENTRIES_C "${CMAKE_BINARY_DIR}/generated/xir_array_entries.c")
add_executable(test_xir_array_entries "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_entries.c")
target_link_libraries(test_xir_array_entries PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${ENTRIES_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_array_entries> 0 "${ENTRIES_C}"
    DEPENDS test_xir_array_entries "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries/transaction/root.xr"
        "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries/transaction/producer.xr" VERBATIM)
add_executable(test_xir_array_entries_native "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_entries.c" "${ENTRIES_C}")
target_link_libraries(test_xir_array_entries_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_array_entries_native PRIVATE XR_ENTRIES_NATIVE=1)
foreach(target test_xir_array_entries test_xir_array_entries_native)
    target_compile_definitions(${target} PRIVATE XR_ENTRIES_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_array_entries_vm COMMAND test_xir_array_entries 0)
add_test(NAME test_xir_array_entries_native COMMAND test_xir_array_entries_native 1)
add_test(NAME test_xir_array_entries_mixed_root COMMAND test_xir_array_entries_native 2)
add_test(NAME test_xir_array_entries_mixed_import COMMAND test_xir_array_entries_native 3)
set_tests_properties(test_xir_array_entries_vm test_xir_array_entries_native test_xir_array_entries_mixed_root
    test_xir_array_entries_mixed_import PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(entries_modes vm native mixed_root mixed_import)
foreach(group IN ITEMS 0 1 2 3 4 5 6 7)
    if(group EQUAL 0)
        set(vm test_xir_array_entries)
        set(native test_xir_array_entries_native)
        set(component transaction)
    else()
        set(component matrix${group})
        set(vm test_xir_array_entries_matrix${group})
        set(native ${vm}_native)
        set(generated "${CMAKE_BINARY_DIR}/generated/xir_array_entries_matrix${group}.c")
        add_executable(${vm} "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_entries.c")
        add_custom_command(OUTPUT "${generated}"
            COMMAND $<TARGET_FILE:${vm}> 0 "${generated}"
            DEPENDS ${vm} "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries/${component}/root.xr"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries/${component}/producer.xr" VERBATIM)
        add_executable(${native} "${CMAKE_SOURCE_DIR}/tests/unit/xir/test_xir_array_entries.c" "${generated}")
        target_compile_definitions(${native} PRIVATE XR_ENTRIES_NATIVE=1)
        foreach(target ${vm} ${native})
            target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
            target_compile_definitions(${target} PRIVATE XR_ENTRIES_MATRIX=${group} XR_ENTRIES_COMPONENT="${component}"
                XR_ENTRIES_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_entries")
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
            PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution")
    endif()
    foreach(mode RANGE 0 3)
        list(GET entries_modes ${mode} suffix)
        if(WIN32)
            add_test(NAME ${vm}_compiler_${suffix}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/unit/xir/run_array_entries_compiler.py"
                    --executable $<TARGET_FILE:${native}> --mode ${mode} --workers 8
                    --reports-root "${CMAKE_BINARY_DIR}/array-entries-compiler/${component}_${suffix}" --component ${component})
        else()
            add_test(NAME ${vm}_compiler_${suffix} COMMAND ${native} --compiler ${mode})
        endif()
        set_tests_properties(${vm}_compiler_${suffix} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE
            LABELS "unit;xir;array;memory;ownership;budget")
        if(WIN32)
            set_tests_properties(${vm}_compiler_${suffix} PROPERTIES PROCESSORS 8)
        endif()
    endforeach()
endforeach()

add_test(NAME test_xir_array_entries_source_rejections COMMAND test_xir_array_entries --rejections)
set_tests_properties(test_xir_array_entries_source_rejections PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;permission")
