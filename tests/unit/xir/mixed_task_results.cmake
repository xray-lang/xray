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
add_executable(test_xir_mixed_task_cancel_prefix xir/test_xir_mixed_task_cancel_prefix.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_cancel_prefix PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_task_prefix_resources xir/test_xir_mixed_task_prefix_resources.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_prefix_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(task_result_target IN ITEMS test_xir_mixed_task_result_producer test_xir_mixed_task_result_lifetime test_xir_mixed_task_result_resources test_xir_mixed_task_source_compiler test_xir_mixed_task_result_pending test_xir_mixed_task_pending_resources test_xir_mixed_task_cancel_prefix test_xir_mixed_task_prefix_resources)
    set_target_properties(${task_result_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_result_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_result_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

add_executable(test_xir_mixed_task_fault_producer xir/test_xir_mixed_task_result_producer.c)
target_link_libraries(test_xir_mixed_task_fault_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_task_fault_producer PRIVATE
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_faults"
    XR_MIXED_TASK_RESULT_MODULES=3)
set(XIR_MIXED_TASK_FAULT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_faults.c")
add_custom_command(OUTPUT "${XIR_MIXED_TASK_FAULT_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_task_fault_producer> "${XIR_MIXED_TASK_FAULT_C}"
    DEPENDS test_xir_mixed_task_fault_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_faults/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_faults/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_task_fault_lifetime xir/test_xir_mixed_task_fault_lifetime.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_lifetime PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
add_executable(test_xir_mixed_task_fault_resources xir/test_xir_mixed_task_fault_resources.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_resources PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_resources PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
add_executable(test_xir_mixed_task_fault_compiler xir/test_xir_mixed_task_source_compiler.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_compiler PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_compiler PRIVATE XR_MIXED_TASK_RESULT_MODULES=3
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_faults")
add_executable(test_xir_mixed_task_fault_prefix xir/test_xir_mixed_task_fault_prefix.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_prefix PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_prefix PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
add_executable(test_xir_mixed_task_fault_prefix_resources xir/test_xir_mixed_task_fault_prefix_resources.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_prefix_resources PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_prefix_resources PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
foreach(task_fault_target IN ITEMS test_xir_mixed_task_fault_producer test_xir_mixed_task_fault_lifetime test_xir_mixed_task_fault_resources test_xir_mixed_task_fault_compiler test_xir_mixed_task_fault_prefix test_xir_mixed_task_fault_prefix_resources)
    set_target_properties(${task_fault_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_fault_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_fault_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
set(XIR_TASK_FAULT_PREFIX_SITES_1 17;17;21;25;35;35;35;35;35;35;35;35;35;35;35;35;35;35;37;37;37;37;46;46;46;46;47;47;47;47;47;47;47;47;48;48;48;50;52;52;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54)
set(XIR_TASK_FAULT_PREFIX_SITES_2 17;17;21;25;28;28;28;30;32;34;35;35;35;35;35;35;35;35;37;37;37;37;40;40;40;40;40;42;44;46;46;46;46;47;48;48;48;50;52;52;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54)
set(XIR_TASK_FAULT_PREFIX_SITES_3 17;17;21;25;28;28;28;30;32;34;35;35;35;35;35;35;35;35;37;37;37;37;40;40;40;40;40;42;44;46;46;46;46;47;48;48;48;50;52;52;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54;54)
foreach(task_fault_backend IN ITEMS 0 1 vm native)
    foreach(task_fault_prefix_mode RANGE 1 3)
        add_test(NAME test_xir_mixed_task_fault_prefix_resources_${task_fault_backend}_${task_fault_prefix_mode}_axes
            COMMAND test_xir_mixed_task_fault_prefix_resources ${task_fault_backend} ${task_fault_prefix_mode})
        set_tests_properties(test_xir_mixed_task_fault_prefix_resources_${task_fault_backend}_${task_fault_prefix_mode}_axes PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix")
        foreach(task_fault_prefix RANGE 0 65)
            list(GET XIR_TASK_FAULT_PREFIX_SITES_${task_fault_prefix_mode} ${task_fault_prefix} task_fault_prefix_sites)
            add_test(NAME test_xir_mixed_task_fault_prefix_resources_${task_fault_backend}_${task_fault_prefix_mode}_fi_${task_fault_prefix}
                COMMAND test_xir_mixed_task_fault_prefix_resources ${task_fault_backend} fi
                    ${task_fault_prefix_mode} ${task_fault_prefix} ${task_fault_prefix_sites})
            set_tests_properties(test_xir_mixed_task_fault_prefix_resources_${task_fault_backend}_${task_fault_prefix_mode}_fi_${task_fault_prefix} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix;fault-injection")
        endforeach()
    endforeach()
    add_test(NAME test_xir_mixed_task_fault_prefix_${task_fault_backend}
        COMMAND test_xir_mixed_task_fault_prefix ${task_fault_backend})
    set_tests_properties(test_xir_mixed_task_fault_prefix_${task_fault_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel")
    foreach(task_fault_compiler_mode IN ITEMS baseline axes)
        add_test(NAME test_xir_mixed_task_fault_compiler_${task_fault_backend}_${task_fault_compiler_mode}
            COMMAND test_xir_mixed_task_fault_compiler ${task_fault_backend} ${task_fault_compiler_mode})
        set_tests_properties(test_xir_mixed_task_fault_compiler_${task_fault_backend}_${task_fault_compiler_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;compiler;resource;generated-c;throw;panic")
    endforeach()
    foreach(task_fault_compiler_shard RANGE 0 274)
        math(EXPR task_fault_compiler_begin "${task_fault_compiler_shard} * 32")
        math(EXPR task_fault_compiler_end "${task_fault_compiler_begin} + 32")
        if(task_fault_compiler_end GREATER 8796)
            set(task_fault_compiler_end 8796)
        endif()
        add_test(NAME test_xir_mixed_task_fault_compiler_${task_fault_backend}_fi_${task_fault_compiler_shard}
            COMMAND test_xir_mixed_task_fault_compiler ${task_fault_backend} fi
                ${task_fault_compiler_begin} ${task_fault_compiler_end} 8796)
        set_tests_properties(test_xir_mixed_task_fault_compiler_${task_fault_backend}_fi_${task_fault_compiler_shard} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;compiler;resource;generated-c;throw;panic;fault-injection")
    endforeach()
    add_test(NAME test_xir_mixed_task_fault_resources_${task_fault_backend}_axes
        COMMAND test_xir_mixed_task_fault_resources ${task_fault_backend} axes)
    set_tests_properties(test_xir_mixed_task_fault_resources_${task_fault_backend}_axes PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;resource")
    add_test(NAME test_xir_mixed_task_fault_resources_${task_fault_backend}_fi
        COMMAND test_xir_mixed_task_fault_resources ${task_fault_backend} fi 56)
    set_tests_properties(test_xir_mixed_task_fault_resources_${task_fault_backend}_fi PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;resource;fault-injection")
    add_test(NAME test_xir_mixed_task_fault_lifetime_${task_fault_backend}
        COMMAND test_xir_mixed_task_fault_lifetime ${task_fault_backend})
    set_tests_properties(test_xir_mixed_task_fault_lifetime_${task_fault_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic")
endforeach()
foreach(task_compiler_backend IN ITEMS 0 1 vm native)
    foreach(task_compiler_mode IN ITEMS baseline axes)
        add_test(NAME test_xir_mixed_task_source_compiler_${task_compiler_backend}_${task_compiler_mode}
            COMMAND test_xir_mixed_task_source_compiler ${task_compiler_backend} ${task_compiler_mode})
        set_tests_properties(test_xir_mixed_task_source_compiler_${task_compiler_backend}_${task_compiler_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;compiler;resource;generated-c")
    endforeach()
    foreach(task_compiler_shard RANGE 0 66)
        math(EXPR task_compiler_begin "${task_compiler_shard} * 64")
        math(EXPR task_compiler_end "${task_compiler_begin} + 64")
        if(task_compiler_end GREATER 4275)
            set(task_compiler_end 4275)
        endif()
        add_test(NAME test_xir_mixed_task_source_compiler_${task_compiler_backend}_fi_${task_compiler_shard}
            COMMAND test_xir_mixed_task_source_compiler ${task_compiler_backend} fi
                ${task_compiler_begin} ${task_compiler_end} 4275)
        set_tests_properties(test_xir_mixed_task_source_compiler_${task_compiler_backend}_fi_${task_compiler_shard} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;compiler;resource;generated-c;fault-injection")
    endforeach()
endforeach()
foreach(task_result_backend IN ITEMS 0 1 vm native)
    set(task_prefix_cancel_sites 35 35 44 44 48 48 48 48 48 48 49 49 49 51 53 53 53 53 53 53 53 53 53)
    set(task_prefix_stop_sites 35 35 38 38 38 40 42 44 44 48 49 49 49 51 53 53 53 53 53 53 53 53 53)
    foreach(task_prefix_mode RANGE 1 3)
        add_test(NAME test_xir_mixed_task_prefix_resources_${task_result_backend}_${task_prefix_mode}_axes
            COMMAND test_xir_mixed_task_prefix_resources ${task_result_backend} ${task_prefix_mode})
        set_tests_properties(test_xir_mixed_task_prefix_resources_${task_result_backend}_${task_prefix_mode}_axes PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;prefix;resource")
        foreach(task_prefix RANGE 0 22)
            if(task_prefix_mode EQUAL 1)
                list(GET task_prefix_cancel_sites ${task_prefix} task_prefix_sites)
            else()
                list(GET task_prefix_stop_sites ${task_prefix} task_prefix_sites)
            endif()
            add_test(NAME test_xir_mixed_task_prefix_resources_${task_result_backend}_${task_prefix_mode}_${task_prefix}_fi
                COMMAND test_xir_mixed_task_prefix_resources ${task_result_backend} fi ${task_prefix_mode} ${task_prefix} ${task_prefix_sites})
            set_tests_properties(test_xir_mixed_task_prefix_resources_${task_result_backend}_${task_prefix_mode}_${task_prefix}_fi PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;prefix;resource;fault-injection")
        endforeach()
    endforeach()
    add_test(NAME test_xir_mixed_task_cancel_prefix_${task_result_backend}
        COMMAND test_xir_mixed_task_cancel_prefix ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_cancel_prefix_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;prefix")
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

add_executable(test_xir_mixed_task_fault_peer_resources xir/test_xir_mixed_task_fault_peer_resources.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_peer_resources PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_peer_resources PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
set_target_properties(test_xir_mixed_task_fault_peer_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_task_fault_peer_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_task_fault_peer_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(task_fault_peer_backend IN ITEMS 0 1 vm native)
    foreach(task_fault_peer_order RANGE 0 1)
        foreach(task_fault_peer_mode RANGE 1 3)
            add_test(NAME test_xir_mixed_task_fault_peer_resources_${task_fault_peer_backend}_${task_fault_peer_order}_${task_fault_peer_mode}
                COMMAND test_xir_mixed_task_fault_peer_resources ${task_fault_peer_backend} ${task_fault_peer_order} ${task_fault_peer_mode})
            set_tests_properties(test_xir_mixed_task_fault_peer_resources_${task_fault_peer_backend}_${task_fault_peer_order}_${task_fault_peer_mode} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix;peer")
            foreach(task_fault_peer_prefix RANGE 0 65)
                list(GET XIR_TASK_FAULT_PREFIX_SITES_${task_fault_peer_mode} ${task_fault_peer_prefix} task_fault_peer_sites)
                add_test(NAME test_xir_mixed_task_fault_peer_resources_${task_fault_peer_backend}_${task_fault_peer_order}_${task_fault_peer_mode}_fi_${task_fault_peer_prefix}
                    COMMAND test_xir_mixed_task_fault_peer_resources ${task_fault_peer_backend} ${task_fault_peer_order} fi ${task_fault_peer_mode} ${task_fault_peer_prefix} ${task_fault_peer_sites})
                set_tests_properties(test_xir_mixed_task_fault_peer_resources_${task_fault_peer_backend}_${task_fault_peer_order}_${task_fault_peer_mode}_fi_${task_fault_peer_prefix} PROPERTIES
                    TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix;peer;fault-injection")
            endforeach()
        endforeach()
    endforeach()
endforeach()

add_executable(test_xir_mixed_task_fault_graph_resources xir/test_xir_mixed_task_fault_graph_resources.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_graph_resources PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_graph_resources PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
set_target_properties(test_xir_mixed_task_fault_graph_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_task_fault_graph_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_task_fault_graph_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(task_fault_graph_backend IN ITEMS 0 1 vm native)
    foreach(task_fault_graph_order RANGE 0 1)
        foreach(task_fault_graph_mode RANGE 1 3)
            add_test(NAME test_xir_mixed_task_fault_graph_resources_${task_fault_graph_backend}_${task_fault_graph_order}_${task_fault_graph_mode}
                COMMAND test_xir_mixed_task_fault_graph_resources ${task_fault_graph_backend} ${task_fault_graph_order} ${task_fault_graph_mode})
            set_tests_properties(test_xir_mixed_task_fault_graph_resources_${task_fault_graph_backend}_${task_fault_graph_order}_${task_fault_graph_mode} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix;peer")
            # The complete normal census adds all 56 peer allocations in every direction and order.
            foreach(task_fault_graph_prefix RANGE 0 65)
                list(GET XIR_TASK_FAULT_PREFIX_SITES_${task_fault_graph_mode} ${task_fault_graph_prefix} task_fault_graph_victim_sites)
                math(EXPR task_fault_graph_sites "${task_fault_graph_victim_sites} + 56")
                set(task_fault_graph_begin 0)
                while(task_fault_graph_begin LESS task_fault_graph_sites)
                    math(EXPR task_fault_graph_end "${task_fault_graph_begin} + 32")
                    if(task_fault_graph_end GREATER task_fault_graph_sites)
                        set(task_fault_graph_end ${task_fault_graph_sites})
                    endif()
                    set(task_fault_graph_name test_xir_mixed_task_fault_graph_resources_${task_fault_graph_backend}_${task_fault_graph_order}_${task_fault_graph_mode}_fi_${task_fault_graph_prefix}_${task_fault_graph_begin}_${task_fault_graph_end})
                    add_test(NAME ${task_fault_graph_name}
                        COMMAND test_xir_mixed_task_fault_graph_resources ${task_fault_graph_backend} ${task_fault_graph_order} fi ${task_fault_graph_mode} ${task_fault_graph_prefix} ${task_fault_graph_begin} ${task_fault_graph_end} ${task_fault_graph_sites})
                    set_tests_properties(${task_fault_graph_name} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 10
                        LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel;resource;prefix;peer;fault-injection")
                    set(task_fault_graph_begin ${task_fault_graph_end})
                endwhile()
            endforeach()
        endforeach()
    endforeach()
endforeach()


add_executable(test_xir_mixed_task_struct_producer xir/test_xir_mixed_task_result_producer.c)
target_link_libraries(test_xir_mixed_task_struct_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_task_struct_producer PRIVATE
    XR_MIXED_TASK_RESULT_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_structs")
set(XIR_MIXED_TASK_STRUCT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_structs.c")
add_custom_command(OUTPUT "${XIR_MIXED_TASK_STRUCT_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_task_struct_producer> "${XIR_MIXED_TASK_STRUCT_C}"
    DEPENDS test_xir_mixed_task_struct_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_structs/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_task_structs/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_task_struct_lifetime xir/test_xir_mixed_task_struct_lifetime.c "${XIR_MIXED_TASK_STRUCT_C}")
target_link_libraries(test_xir_mixed_task_struct_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(task_struct_target IN ITEMS test_xir_mixed_task_struct_producer test_xir_mixed_task_struct_lifetime)
    target_compile_definitions(${task_struct_target} PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
    set_target_properties(${task_struct_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_struct_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_struct_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(task_struct_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_struct_lifetime_${task_struct_backend}
        COMMAND test_xir_mixed_task_struct_lifetime ${task_struct_backend})
    set_tests_properties(test_xir_mixed_task_struct_lifetime_${task_struct_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;struct;ownership;generated-c;lifecycle;atomic;cow")
endforeach()


add_executable(test_xir_mixed_task_struct_control xir/test_xir_mixed_task_struct_control.c "${XIR_MIXED_TASK_STRUCT_C}")
target_link_libraries(test_xir_mixed_task_struct_control PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_struct_control PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
set_target_properties(test_xir_mixed_task_struct_control PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_task_struct_control PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_task_struct_control PRIVATE -Wall -Wextra -Werror)
endif()
foreach(task_struct_control_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_struct_control_${task_struct_control_backend}
        COMMAND test_xir_mixed_task_struct_control ${task_struct_control_backend})
    set_tests_properties(test_xir_mixed_task_struct_control_${task_struct_control_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;struct;ownership;generated-c;lifecycle;atomic;cow;cancel;prefix")
endforeach()


add_executable(test_xir_mixed_task_struct_resources xir/test_xir_mixed_task_struct_resources.c "${XIR_MIXED_TASK_STRUCT_C}")
target_link_libraries(test_xir_mixed_task_struct_resources PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_struct_resources PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
set_target_properties(test_xir_mixed_task_struct_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_task_struct_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_task_struct_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(task_struct_resource_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_struct_resources_${task_struct_resource_backend}_axes
        COMMAND test_xir_mixed_task_struct_resources ${task_struct_resource_backend} axes)
    set_tests_properties(test_xir_mixed_task_struct_resources_${task_struct_resource_backend}_axes PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;struct;ownership;generated-c;lifecycle;atomic;cow;cancel;resource;axes")
endforeach()

set(XIR_TASK_STRUCT_RESOURCE_SITES 63 57 45 45)
foreach(task_struct_resource_backend IN ITEMS 0 1 vm native)
    foreach(task_struct_resource_mode RANGE 0 3)
        list(GET XIR_TASK_STRUCT_RESOURCE_SITES ${task_struct_resource_mode} task_struct_resource_sites)
        add_test(NAME test_xir_mixed_task_struct_resources_${task_struct_resource_backend}_fi_${task_struct_resource_mode}
            COMMAND test_xir_mixed_task_struct_resources ${task_struct_resource_backend} fi ${task_struct_resource_mode} ${task_struct_resource_sites})
        set_tests_properties(test_xir_mixed_task_struct_resources_${task_struct_resource_backend}_fi_${task_struct_resource_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;struct;ownership;generated-c;lifecycle;atomic;cow;cancel;resource;fault-injection")
    endforeach()
endforeach()

add_executable(test_xir_mixed_missing_families xir/test_xir_mixed_missing_families.c)
target_link_libraries(test_xir_mixed_missing_families PRIVATE xray_xir_source)
target_compile_definitions(test_xir_mixed_missing_families PRIVATE
    XR_MIXED_MISSING_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_missing_families")
set_target_properties(test_xir_mixed_missing_families PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_missing_families PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_missing_families PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(test_xir_mixed_cell_cycle_producer xir/test_xir_mixed_cell_cycle_producer.c)
target_link_libraries(test_xir_mixed_cell_cycle_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_cell_cycle_producer PRIVATE
    XR_CELL_CYCLE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cell_cycles")
set(XIR_MIXED_CELL_CYCLE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_cell_cycles.c")
add_custom_command(OUTPUT "${XIR_MIXED_CELL_CYCLE_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_cell_cycle_producer> "${XIR_MIXED_CELL_CYCLE_C}"
    DEPENDS test_xir_mixed_cell_cycle_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cell_cycles/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cell_cycles/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_cell_cycles xir/test_xir_mixed_cell_cycles.c "${XIR_MIXED_CELL_CYCLE_C}")
target_link_libraries(test_xir_mixed_cell_cycles PRIVATE xray_xir_vm xray_xir_scalar)
foreach(cell_cycle_target IN ITEMS test_xir_mixed_cell_cycle_producer test_xir_mixed_cell_cycles)
    set_target_properties(${cell_cycle_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${cell_cycle_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${cell_cycle_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(cell_cycle_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_cell_cycles_${cell_cycle_backend} COMMAND test_xir_mixed_cell_cycles ${cell_cycle_backend})
    set_tests_properties(test_xir_mixed_cell_cycles_${cell_cycle_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;closure;cell;cycle")
endforeach()

add_executable(test_xir_mixed_callback_boundary_producer xir/test_xir_mixed_callback_boundary_producer.c)
target_link_libraries(test_xir_mixed_callback_boundary_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_callback_boundary_producer PRIVATE
    XR_CALLBACK_BOUNDARY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary")
set(XIR_MIXED_CALLBACK_BOUNDARY_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_callback_boundary.c")
add_custom_command(OUTPUT "${XIR_MIXED_CALLBACK_BOUNDARY_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_callback_boundary_producer> "${XIR_MIXED_CALLBACK_BOUNDARY_C}"
    DEPENDS test_xir_mixed_callback_boundary_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_callback_boundary/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_callback_boundary xir/test_xir_mixed_callback_boundary.c "${XIR_MIXED_CALLBACK_BOUNDARY_C}")
target_link_libraries(test_xir_mixed_callback_boundary PRIVATE xray_xir_vm xray_xir_scalar)
foreach(callback_boundary_target IN ITEMS test_xir_mixed_callback_boundary_producer test_xir_mixed_callback_boundary)
    set_target_properties(${callback_boundary_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${callback_boundary_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${callback_boundary_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(callback_boundary_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_callback_boundary_${callback_boundary_backend} COMMAND test_xir_mixed_callback_boundary ${callback_boundary_backend})
    set_tests_properties(test_xir_mixed_callback_boundary_${callback_boundary_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;task;callback;reentry")
endforeach()

add_executable(test_xir_mixed_iterator_producer xir/test_xir_mixed_iterator_producer.c)
target_link_libraries(test_xir_mixed_iterator_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_iterator_producer PRIVATE
    XR_ITERATOR_LIFETIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_MIXED_ITERATOR_LIFETIME_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_iterator_lifetime.c")
add_custom_command(OUTPUT "${XIR_MIXED_ITERATOR_LIFETIME_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_iterator_producer> "${XIR_MIXED_ITERATOR_LIFETIME_C}"
    DEPENDS test_xir_mixed_iterator_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_iterator_lifetime xir/test_xir_mixed_iterator_lifetime.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(iterator_lifetime_target IN ITEMS test_xir_mixed_iterator_producer test_xir_mixed_iterator_lifetime)
    set_target_properties(${iterator_lifetime_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${iterator_lifetime_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${iterator_lifetime_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(iterator_lifetime_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_iterator_lifetime_${iterator_lifetime_backend} COMMAND test_xir_mixed_iterator_lifetime ${iterator_lifetime_backend})
    set_tests_properties(test_xir_mixed_iterator_lifetime_${iterator_lifetime_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;cell;timer;cleanup")
endforeach()

add_executable(test_xir_mixed_iterator_interleave xir/test_xir_mixed_iterator_interleave.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_interleave PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_iterator_interleave PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_iterator_interleave PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_iterator_interleave PRIVATE -Wall -Wextra -Werror)
endif()
foreach(iterator_pair_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_iterator_interleave_${iterator_pair_backend} COMMAND test_xir_mixed_iterator_interleave ${iterator_pair_backend})
    set_tests_properties(test_xir_mixed_iterator_interleave_${iterator_pair_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;cell;timer;cleanup;interleave;reentry")
endforeach()

add_executable(test_xir_mixed_iterator_source_compiler xir/test_xir_mixed_iterator_source_compiler.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_source_compiler PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_iterator_source_compiler PRIVATE
    XR_ITERATOR_LIFETIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_mixed_iterator_source_compiler PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_iterator_source_compiler PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_iterator_source_compiler PRIVATE -Wall -Wextra -Werror)
endif()
foreach(iterator_compiler_backend IN ITEMS 0 1 vm native)
    foreach(iterator_compiler_mode IN ITEMS baseline axes)
        add_test(NAME test_xir_mixed_iterator_source_compiler_${iterator_compiler_backend}_${iterator_compiler_mode}
            COMMAND test_xir_mixed_iterator_source_compiler ${iterator_compiler_backend} ${iterator_compiler_mode})
        set_tests_properties(test_xir_mixed_iterator_source_compiler_${iterator_compiler_backend}_${iterator_compiler_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;generator;iterator;compiler;resource")
    endforeach()
endforeach()

foreach(iterator_compiler_backend IN ITEMS 0 1 vm native)
    foreach(iterator_compiler_begin RANGE 0 8582 64)
        math(EXPR iterator_compiler_end "${iterator_compiler_begin} + 64")
        if(iterator_compiler_end GREATER 8583)
            set(iterator_compiler_end 8583)
        endif()
        add_test(NAME test_xir_mixed_iterator_source_compiler_${iterator_compiler_backend}_fi_${iterator_compiler_begin}_${iterator_compiler_end}
            COMMAND test_xir_mixed_iterator_source_compiler ${iterator_compiler_backend} fi ${iterator_compiler_begin} ${iterator_compiler_end} 8583)
        set_tests_properties(test_xir_mixed_iterator_source_compiler_${iterator_compiler_backend}_fi_${iterator_compiler_begin}_${iterator_compiler_end} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;generator;iterator;compiler;resource;fault_injection")
    endforeach()
endforeach()
