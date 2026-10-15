foreach(flavor scalar constructed)
    string(TOUPPER "${flavor}" upper)
    set(parameter_dir "${CMAKE_CURRENT_BINARY_DIR}/task-${flavor}-parameters")
    set(parameter_fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_${flavor}_parameters")
    set(parameter_c "${parameter_dir}/task_${flavor}_native.c")
    set(parameter_header "${parameter_dir}/task_${flavor}_generated.h")
    set(parameter_emit "test_xir_task_parameter_${flavor}_emit")
    set(parameter_runner "test_xir_task_parameter_${flavor}_results")
    set(parameter_control "test_xir_task_parameter_${flavor}_control")
    add_executable(${parameter_emit} "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_${flavor}_emit.c")
    target_link_libraries(${parameter_emit} PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
    add_custom_command(OUTPUT "${parameter_c}" "${parameter_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${parameter_dir}"
        COMMAND ${parameter_emit} "${parameter_c}" "${parameter_header}"
        DEPENDS ${parameter_emit} "${parameter_fixture}/root.xr" VERBATIM)
    add_executable(${parameter_runner} "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_${flavor}_results.c"
        "${parameter_c}" "${parameter_header}")
    target_include_directories(${parameter_runner} PRIVATE "${parameter_dir}")
    target_link_libraries(${parameter_runner} PRIVATE xray_xir_source xray_xir_vm)
    add_executable(${parameter_control} "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_${flavor}_control.c"
        "${parameter_c}" "${parameter_header}")
    target_include_directories(${parameter_control} PRIVATE "${parameter_dir}")
    target_link_libraries(${parameter_control} PRIVATE xray_xir_source xray_xir_vm)
    foreach(target ${parameter_emit} ${parameter_runner} ${parameter_control})
        target_compile_definitions(${target} PRIVATE XR_TASK_PARAMETER_FIXTURE=1
            XR_TASK_${upper}_FIXTURES="${parameter_fixture}" XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    if(flavor STREQUAL "scalar")
        set(parameter_families i8 i16 i32 u8 u16 u32 u64 f32 f64 rune i64 bool string unit)
    else()
        set(parameter_families array nested nullable nullable_array tuple enum atomic task)
    endif()
    foreach(family IN LISTS parameter_families)
        foreach(backend vm native vm-native native-vm)
            string(REPLACE "-" "_" name "${backend}")
            foreach(mode normal fi)
                add_test(NAME test_xir_task_parameter_${flavor}_${family}_${name}_${mode}
                    COMMAND ${parameter_runner} ${family} ${backend} ${mode})
                set_tests_properties(test_xir_task_parameter_${flavor}_${family}_${name}_${mode} PROPERTIES
                    TIMEOUT 60 LABELS "unit;xir;task;ownership;runtime;generated-c;parameters")
            endforeach()
            add_test(NAME test_xir_task_parameter_${flavor}_control_${family}_${name}
                COMMAND ${parameter_control} ${family} ${backend})
            set_tests_properties(test_xir_task_parameter_${flavor}_control_${family}_${name} PROPERTIES
                TIMEOUT 60 LABELS "unit;xir;task;ownership;runtime;generated-c;parameters;cancel")
        endforeach()
    endforeach()
endforeach()
add_executable(test_xir_task_parameter_rejects "${CMAKE_CURRENT_LIST_DIR}/test_xir_task_constructed_rejects.c")
target_link_libraries(test_xir_task_parameter_rejects PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_task_parameter_rejects PRIVATE
    XR_TASK_CONSTRUCTED_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p08_task_constructed_parameters"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_task_parameter_rejects PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_parameter_rejects PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case array_invariance tuple_invariance class nested_class root_worker generic_bound ref
    struct_class struct_callable struct_error_variant)
    add_test(NAME test_xir_task_parameter_reject_${case} COMMAND test_xir_task_parameter_rejects ${case})
    set_tests_properties(test_xir_task_parameter_reject_${case} PROPERTIES
        TIMEOUT 60 LABELS "unit;xir;task;ownership;source;reject;parameters")
endforeach()
