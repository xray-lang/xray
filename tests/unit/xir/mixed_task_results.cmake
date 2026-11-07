

set(XIR_MIXED_TASK_RESULT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_results.c")

set(XIR_MIXED_TASK_FAULT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_faults.c")

set(XIR_MIXED_TASK_STRUCT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_task_structs.c")

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

foreach(cell_cycle_target IN ITEMS test_xir_mixed_cell_cycle_producer)
    set_target_properties(${cell_cycle_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${cell_cycle_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${cell_cycle_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/mixed_callback_boundary.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/mixed_iterator_lifetime.cmake")

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

    endforeach()
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

foreach(forin_target IN ITEMS test_xir_mixed_forin_producer)
    set_target_properties(${forin_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${forin_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${forin_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

set(forin_prefix_polls 52 88 50 95 66 64 118 102 130 129 116 143)
foreach(forin_prefix_backend IN ITEMS 0 1 vm native)
    foreach(forin_prefix_scenario RANGE 0 11)
        list(GET forin_prefix_polls ${forin_prefix_scenario} forin_prefix_last)
        foreach(forin_prefix_mode RANGE 0 2)
            foreach(forin_prefix_close RANGE 0 1)
                foreach(forin_prefix_drop RANGE 0 1)
                    set(forin_prefix_name test_xir_mixed_forin_prefixes_${forin_prefix_backend}_${forin_prefix_scenario}_${forin_prefix_mode}_${forin_prefix_close}_${forin_prefix_drop})

                endforeach()
            endforeach()
        endforeach()
    endforeach()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/mixed_atomic.cmake")
