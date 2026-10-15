add_executable(test_xir_mixed_task_result_producer xir/test_xir_mixed_task_result_producer.c)
target_link_libraries(test_xir_mixed_task_result_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_task_result_producer PRIVATE
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results")
set(XIR_MIXED_TASK_RESULT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_results.c")
add_custom_command(OUTPUT "${XIR_MIXED_TASK_RESULT_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_task_result_producer> "${XIR_MIXED_TASK_RESULT_C}"
    DEPENDS test_xir_mixed_task_result_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_task_result_lifetime xir/test_xir_mixed_task_result_lifetime.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_result_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_task_result_resources xir/test_xir_mixed_task_result_resources.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_result_resources PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_task_source_compiler xir/test_xir_mixed_task_source_compiler.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_source_compiler PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_source_compiler PRIVATE
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_results")
add_executable(test_xir_mixed_task_result_pending xir/test_xir_mixed_task_result_pending.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_result_pending PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_task_pending_resources xir/test_xir_mixed_task_pending_resources.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_pending_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(task_result_target IN ITEMS test_xir_mixed_task_result_producer test_xir_mixed_task_result_lifetime test_xir_mixed_task_result_resources test_xir_mixed_task_source_compiler test_xir_mixed_task_result_pending test_xir_mixed_task_pending_resources)
    set_target_properties(${task_result_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_result_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_result_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(task_compiler_backend IN ITEMS 0 1 vm native)
    foreach(task_compiler_mode IN ITEMS baseline axes)
        add_test(NAME test_xir_mixed_task_source_compiler_${task_compiler_backend}_${task_compiler_mode}
            COMMAND test_xir_mixed_task_source_compiler ${task_compiler_backend} ${task_compiler_mode})
        set_tests_properties(test_xir_mixed_task_source_compiler_${task_compiler_backend}_${task_compiler_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;compiler;resource;generated-c")
    endforeach()
    foreach(task_compiler_shard RANGE 0 71)
        math(EXPR task_compiler_begin "${task_compiler_shard} * 64")
        math(EXPR task_compiler_end "${task_compiler_begin} + 64")
        if(task_compiler_end GREATER 4578)
            set(task_compiler_end 4578)
        endif()
        add_test(NAME test_xir_mixed_task_source_compiler_${task_compiler_backend}_fi_${task_compiler_shard}
            COMMAND test_xir_mixed_task_source_compiler ${task_compiler_backend} fi
                ${task_compiler_begin} ${task_compiler_end} 4578)
        set_tests_properties(test_xir_mixed_task_source_compiler_${task_compiler_backend}_fi_${task_compiler_shard} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;compiler;resource;generated-c;fault-injection")
    endforeach()
endforeach()
foreach(task_result_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_pending_resources_${task_result_backend}_axes
        COMMAND test_xir_mixed_task_pending_resources ${task_result_backend} axes)
    set_tests_properties(test_xir_mixed_task_pending_resources_${task_result_backend}_axes PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;resource")
    set(task_pending_mode_sites 53 48 38 38)
    foreach(task_pending_mode RANGE 0 3)
        list(GET task_pending_mode_sites ${task_pending_mode} task_pending_sites)
        add_test(NAME test_xir_mixed_task_pending_resources_${task_result_backend}_${task_pending_mode}_fi
            COMMAND test_xir_mixed_task_pending_resources ${task_result_backend} fi ${task_pending_mode} ${task_pending_sites})
        set_tests_properties(test_xir_mixed_task_pending_resources_${task_result_backend}_${task_pending_mode}_fi PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;resource;fault-injection")
    endforeach()
    add_test(NAME test_xir_mixed_task_result_pending_${task_result_backend}
        COMMAND test_xir_mixed_task_result_pending ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_result_pending_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel")
    add_test(NAME test_xir_mixed_task_result_lifetime_${task_result_backend}
        COMMAND test_xir_mixed_task_result_lifetime ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_result_lifetime_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound")
    foreach(task_result_mode IN ITEMS axes fi)
        set(task_result_expected)
        if(task_result_mode STREQUAL "fi")
            set(task_result_expected 53)
        endif()
        add_test(NAME test_xir_mixed_task_result_resources_${task_result_backend}_${task_result_mode}
            COMMAND test_xir_mixed_task_result_resources ${task_result_backend} ${task_result_mode} ${task_result_expected})
        set_tests_properties(test_xir_mixed_task_result_resources_${task_result_backend}_${task_result_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;resource")
    endforeach()
endforeach()
