add_executable(test_xir_task_scalar_emit ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_scalar_emit.c)
target_link_libraries(test_xir_task_scalar_emit PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
target_compile_definitions(test_xir_task_scalar_emit PRIVATE
    XR_TASK_SCALAR_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_scalars"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_scalar_emit PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_scalar_emit PRIVATE -Wall -Wextra -Werror)
endif()
set(TASK_SCALAR_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-scalar-results")
set(TASK_SCALAR_C "${TASK_SCALAR_DIR}/task_scalar_native.c")
set(TASK_SCALAR_HEADER "${TASK_SCALAR_DIR}/task_scalar_generated.h")
add_custom_command(OUTPUT "${TASK_SCALAR_C}" "${TASK_SCALAR_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TASK_SCALAR_DIR}"
    COMMAND test_xir_task_scalar_emit "${TASK_SCALAR_C}" "${TASK_SCALAR_HEADER}"
    DEPENDS test_xir_task_scalar_emit "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_scalars/root.xr" VERBATIM)
add_executable(test_xir_task_scalar_results ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_scalar_results.c
    "${TASK_SCALAR_C}" "${TASK_SCALAR_HEADER}")
target_include_directories(test_xir_task_scalar_results PRIVATE "${TASK_SCALAR_DIR}")
target_link_libraries(test_xir_task_scalar_results PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_scalar_results PRIVATE
    XR_TASK_SCALAR_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_scalars"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_scalar_results PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_scalar_results PRIVATE -Wall -Wextra -Werror)
endif()
foreach(family i8 i16 i32 u8 u16 u32 u64 f32 f64 rune i64 bool string unit)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")
        foreach(mode normal fi)
            add_test(NAME test_xir_task_scalar_${family}_${name}_${mode}
                COMMAND test_xir_task_scalar_results ${family} ${backend} ${mode})
            set_tests_properties(test_xir_task_scalar_${family}_${name}_${mode} PROPERTIES TIMEOUT 60
                LABELS "unit;xir;task;ownership;runtime;generated-c")
        endforeach()
    endforeach()
endforeach()
add_executable(test_xir_task_scalar_control ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_scalar_control.c
    "${TASK_SCALAR_C}" "${TASK_SCALAR_HEADER}")
add_executable(test_xir_task_scalar_rejects ${CMAKE_CURRENT_LIST_DIR}/test_xir_task_scalar_rejects.c)
foreach(target test_xir_task_scalar_control test_xir_task_scalar_rejects)
    target_include_directories(${target} PRIVATE "${TASK_SCALAR_DIR}")
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm)
    target_compile_definitions(${target} PRIVATE
        XR_TASK_SCALAR_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_scalars"
        XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(family i8 i16 i32 u8 u16 u32 u64 f32 f64 rune i64 bool string unit)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")
        add_test(NAME test_xir_task_scalar_control_${family}_${name} COMMAND test_xir_task_scalar_control ${family} ${backend})
        set_tests_properties(test_xir_task_scalar_control_${family}_${name} PROPERTIES TIMEOUT 60
            LABELS "unit;xir;task;ownership;runtime;cancel;generated-c")
    endforeach()
endforeach()
foreach(case invariance float_invariance construct non_task)
    add_test(NAME test_xir_task_scalar_reject_${case} COMMAND test_xir_task_scalar_rejects ${case})
    set_tests_properties(test_xir_task_scalar_reject_${case} PROPERTIES TIMEOUT 60 LABELS "unit;xir;task;source;reject;ownership")
endforeach()
