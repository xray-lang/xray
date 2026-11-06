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
add_executable(test_xir_mixed_task_result_pending xir/test_xir_mixed_task_result_pending.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_result_pending PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_task_cancel_prefix xir/test_xir_mixed_task_cancel_prefix.c "${XIR_MIXED_TASK_RESULT_C}")
target_link_libraries(test_xir_mixed_task_cancel_prefix PRIVATE xray_xir_vm xray_xir_scalar)
foreach(task_result_target IN ITEMS test_xir_mixed_task_result_producer test_xir_mixed_task_result_lifetime test_xir_mixed_task_result_pending test_xir_mixed_task_cancel_prefix)
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
add_executable(test_xir_mixed_task_fault_prefix xir/test_xir_mixed_task_fault_prefix.c "${XIR_MIXED_TASK_FAULT_C}")
target_link_libraries(test_xir_mixed_task_fault_prefix PRIVATE xray_xir_vm xray_xir_scalar)
target_compile_definitions(test_xir_mixed_task_fault_prefix PRIVATE XR_MIXED_TASK_RESULT_MODULES=3)
foreach(task_fault_target IN ITEMS test_xir_mixed_task_fault_producer test_xir_mixed_task_fault_lifetime test_xir_mixed_task_fault_prefix)
    set_target_properties(${task_fault_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${task_fault_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${task_fault_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(task_fault_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_fault_prefix_${task_fault_backend}
        COMMAND test_xir_mixed_task_fault_prefix ${task_fault_backend})
    set_tests_properties(test_xir_mixed_task_fault_prefix_${task_fault_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic;cancel")
    add_test(NAME test_xir_mixed_task_fault_lifetime_${task_fault_backend}
        COMMAND test_xir_mixed_task_fault_lifetime ${task_fault_backend})
    set_tests_properties(test_xir_mixed_task_fault_lifetime_${task_fault_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;throw;panic")
endforeach()
foreach(task_result_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_task_cancel_prefix_${task_result_backend}
        COMMAND test_xir_mixed_task_cancel_prefix ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_cancel_prefix_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel;prefix")
    add_test(NAME test_xir_mixed_task_result_pending_${task_result_backend}
        COMMAND test_xir_mixed_task_result_pending ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_result_pending_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound;cancel")
    add_test(NAME test_xir_mixed_task_result_lifetime_${task_result_backend}
        COMMAND test_xir_mixed_task_result_lifetime ${task_result_backend})
    set_tests_properties(test_xir_mixed_task_result_lifetime_${task_result_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;task;ownership;generated-c;lifecycle;compound")
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
include("${CMAKE_CURRENT_LIST_DIR}/mixed_callback_boundary.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/mixed_iterator_lifetime.cmake")
add_executable(test_xir_mixed_iterator_index_boundaries xir/test_xir_mixed_iterator_index_boundaries.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_index_boundaries PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_iterator_index_boundaries PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_iterator_index_boundaries PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_iterator_index_boundaries PRIVATE -Wall -Wextra -Werror)
endif()
foreach(iterator_index_backend IN ITEMS 0 1 vm native)
    foreach(iterator_index_mode IN ITEMS baseline axes)
        add_test(NAME test_xir_mixed_iterator_index_boundaries_${iterator_index_backend}_${iterator_index_mode}
            COMMAND test_xir_mixed_iterator_index_boundaries ${iterator_index_backend} ${iterator_index_mode})
        set_tests_properties(test_xir_mixed_iterator_index_boundaries_${iterator_index_backend}_${iterator_index_mode} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;resource;timer;cleanup;nth;bounds")
    endforeach()
endforeach()
foreach(iterator_index_case IN ITEMS
    "0,0,0,0,96"
    "0,0,0,1,96"
    "0,0,1,0,102"
    "0,0,1,1,102"
    "0,0,2,0,105"
    "0,0,2,1,105"
    "0,0,3,0,114"
    "0,0,3,1,114"
    "0,1,0,0,96"
    "0,1,0,1,96"
    "0,1,1,0,102"
    "0,1,1,1,102"
    "0,1,2,0,105"
    "0,1,2,1,105"
    "0,1,3,0,114"
    "0,1,3,1,114"
    "1,0,0,0,96"
    "1,0,0,1,96"
    "1,0,1,0,102"
    "1,0,1,1,102"
    "1,0,2,0,105"
    "1,0,2,1,105"
    "1,0,3,0,114"
    "1,0,3,1,114"
    "1,1,0,0,96"
    "1,1,0,1,96"
    "1,1,1,0,102"
    "1,1,1,1,102"
    "1,1,2,0,105"
    "1,1,2,1,105"
    "1,1,3,0,114"
    "1,1,3,1,114"
    "vm,0,0,0,96"
    "vm,0,0,1,96"
    "vm,0,1,0,102"
    "vm,0,1,1,102"
    "vm,0,2,0,105"
    "vm,0,2,1,105"
    "vm,0,3,0,114"
    "vm,0,3,1,114"
    "vm,1,0,0,96"
    "vm,1,0,1,96"
    "vm,1,1,0,102"
    "vm,1,1,1,102"
    "vm,1,2,0,105"
    "vm,1,2,1,105"
    "vm,1,3,0,114"
    "vm,1,3,1,114"
    "native,0,0,0,96"
    "native,0,0,1,96"
    "native,0,1,0,102"
    "native,0,1,1,102"
    "native,0,2,0,105"
    "native,0,2,1,105"
    "native,0,3,0,114"
    "native,0,3,1,114"
    "native,1,0,0,96"
    "native,1,0,1,96"
    "native,1,1,0,102"
    "native,1,1,1,102"
    "native,1,2,0,105"
    "native,1,2,1,105"
    "native,1,3,0,114"
    "native,1,3,1,114"
)
    string(REPLACE "," ";" iterator_index_fields "${iterator_index_case}")
    list(GET iterator_index_fields 0 iterator_index_backend)
    list(GET iterator_index_fields 1 iterator_index_victim)
    list(GET iterator_index_fields 2 iterator_index_stage)
    list(GET iterator_index_fields 3 iterator_index_maximum)
    list(GET iterator_index_fields 4 iterator_index_sites)
    math(EXPR iterator_index_last "${iterator_index_sites} - 1")
    foreach(iterator_index_begin RANGE 0 ${iterator_index_last} 64)
        math(EXPR iterator_index_end "${iterator_index_begin} + 64")
        if(iterator_index_end GREATER iterator_index_sites)
            set(iterator_index_end ${iterator_index_sites})
        endif()
        add_test(NAME test_xir_mixed_iterator_index_boundaries_${iterator_index_backend}_fi_${iterator_index_victim}_${iterator_index_stage}_${iterator_index_maximum}_${iterator_index_begin}_${iterator_index_end}
            COMMAND test_xir_mixed_iterator_index_boundaries ${iterator_index_backend} fi ${iterator_index_victim} ${iterator_index_stage} ${iterator_index_maximum} ${iterator_index_begin} ${iterator_index_end} ${iterator_index_sites})
        set_tests_properties(test_xir_mixed_iterator_index_boundaries_${iterator_index_backend}_fi_${iterator_index_victim}_${iterator_index_stage}_${iterator_index_maximum}_${iterator_index_begin}_${iterator_index_end} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;resource;timer;cleanup;nth;bounds;fault_injection")
    endforeach()
endforeach()
add_executable(test_xir_mixed_iterator_pull_prefixes xir/test_xir_mixed_iterator_pull_prefixes.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_pull_prefixes PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_iterator_pull_prefixes PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_iterator_pull_prefixes PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_iterator_pull_prefixes PRIVATE -Wall -Wextra -Werror)
endif()
foreach(iterator_prefix_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_iterator_pull_prefixes_${iterator_prefix_backend}
        COMMAND test_xir_mixed_iterator_pull_prefixes ${iterator_prefix_backend})
    set_tests_properties(test_xir_mixed_iterator_pull_prefixes_${iterator_prefix_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;timer;cleanup;nth;cancel;prefix")
endforeach()
add_executable(test_xir_mixed_cycle_batch_producer xir/test_xir_mixed_cycle_batch_producer.c)
target_link_libraries(test_xir_mixed_cycle_batch_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_cycle_batch_producer PRIVATE
    XR_CYCLE_BATCH_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures")
set_target_properties(test_xir_mixed_cycle_batch_producer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_cycle_batch_producer PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_cycle_batch_producer PRIVATE -Wall -Wextra -Werror)
endif()
set(XIR_MIXED_CYCLE_BATCH_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_cycle_batch.c")
add_custom_command(OUTPUT "${XIR_MIXED_CYCLE_BATCH_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_cycle_batch_producer> "${XIR_MIXED_CYCLE_BATCH_C}"
    DEPENDS test_xir_mixed_cycle_batch_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cycle_batch/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cycle_batch/branch.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_cell_cycles/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_cycle_batch_policy xir/test_xir_mixed_cycle_batch_policy.c "${XIR_MIXED_CYCLE_BATCH_C}")
target_link_libraries(test_xir_mixed_cycle_batch_policy PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_cycle_batch_policy PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_cycle_batch_policy PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_cycle_batch_policy PRIVATE -Wall -Wextra -Werror)
endif()
foreach(cycle_batch_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_cycle_batch_policy_${cycle_batch_backend} COMMAND test_xir_mixed_cycle_batch_policy ${cycle_batch_backend})
    set_tests_properties(test_xir_mixed_cycle_batch_policy_${cycle_batch_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;closure;cell;cycle;branch;policy")
endforeach()
add_executable(test_xir_mixed_forin_producer xir/test_xir_mixed_forin_producer.c)
target_link_libraries(test_xir_mixed_forin_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_forin_producer PRIVATE
    XR_ITERATOR_LIFETIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_forin"
    XR_ITERATOR_FORIN_AUTHORITY="${PROJECT_SOURCE_DIR}/tests/fixtures"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_MIXED_FORIN_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_forin.c")
add_custom_command(OUTPUT "${XIR_MIXED_FORIN_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_forin_producer> "${XIR_MIXED_FORIN_C}"
    DEPENDS test_xir_mixed_forin_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_forin/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_forin/worker.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_forin_lifetime xir/test_xir_mixed_forin_lifetime.c "${XIR_MIXED_FORIN_C}")
target_link_libraries(test_xir_mixed_forin_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(forin_target IN ITEMS test_xir_mixed_forin_producer test_xir_mixed_forin_lifetime)
    set_target_properties(${forin_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${forin_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${forin_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(forin_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_forin_lifetime_${forin_backend} COMMAND test_xir_mixed_forin_lifetime ${forin_backend})
    set_tests_properties(test_xir_mixed_forin_lifetime_${forin_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 10 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;forin;cleanup;timer")
endforeach()
add_executable(test_xir_mixed_forin_prefixes xir/test_xir_mixed_forin_prefixes.c "${XIR_MIXED_FORIN_C}")
target_link_libraries(test_xir_mixed_forin_prefixes PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_forin_prefixes PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_forin_prefixes PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_forin_prefixes PRIVATE -Wall -Wextra -Werror)
endif()
set(forin_prefix_polls 52 88 50 95 66 64 118 102 130 129 116 143)
foreach(forin_prefix_backend IN ITEMS 0 1 vm native)
    foreach(forin_prefix_scenario RANGE 0 11)
        list(GET forin_prefix_polls ${forin_prefix_scenario} forin_prefix_last)
        foreach(forin_prefix_mode RANGE 0 2)
            foreach(forin_prefix_close RANGE 0 1)
                foreach(forin_prefix_drop RANGE 0 1)
                    set(forin_prefix_name test_xir_mixed_forin_prefixes_${forin_prefix_backend}_${forin_prefix_scenario}_${forin_prefix_mode}_${forin_prefix_close}_${forin_prefix_drop})
                    add_test(NAME ${forin_prefix_name} COMMAND test_xir_mixed_forin_prefixes ${forin_prefix_backend} suite ${forin_prefix_scenario} ${forin_prefix_mode} ${forin_prefix_last} ${forin_prefix_close} ${forin_prefix_drop})
                    set_tests_properties(${forin_prefix_name} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 20
                        LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;forin;cleanup;timer;cancel;prefix")
                endforeach()
            endforeach()
        endforeach()
    endforeach()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/mixed_atomic.cmake")
