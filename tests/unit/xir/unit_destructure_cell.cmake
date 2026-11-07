# Both Source and Checked Catalog consumers execute independent state oracles.
set(XIR_UNIT_DESTRUCTURE_CELL_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_source.xrc)
set(XIR_UNIT_DESTRUCTURE_CELL_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_catalog.xrc)
set(XIR_UNIT_DESTRUCTURE_CELL_PENDING ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_pending.xrc)
add_library(xir_unit_destructure_cell_source_runtime OBJECT xir/test_xir_unit_destructure_cell_execution.c)
target_compile_definitions(xir_unit_destructure_cell_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_unit_destructure_cell_source_runtime PRIVATE xray_xir_vm)

file(GLOB XIR_UNIT_DESTRUCTURE_CELL_FIXTURES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_destructure_cell/*.xr")

set(XIR_UNIT_DESTRUCTURE_CELL_TARGETS xir_unit_destructure_cell_source_runtime)
foreach(state_flow IN ITEMS source catalog pending)
    if(state_flow STREQUAL "source")
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_SOURCE})
    elseif(state_flow STREQUAL "catalog")
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_CATALOG})
    else()
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_PENDING})
    endif()
    set(state_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_${state_flow}.c)

    set(state_pending_arg)
    if(state_flow STREQUAL "pending")
        set(state_pending_arg --pending)
    endif()

    foreach(state_mode IN ITEMS native mixed)
        set(state_target test_xir_unit_destructure_cell_${state_flow}_${state_mode})

        list(APPEND XIR_UNIT_DESTRUCTURE_CELL_TARGETS)

    endforeach()

endforeach()
foreach(state_target IN LISTS XIR_UNIT_DESTRUCTURE_CELL_TARGETS)
    if(MSVC)
        target_compile_options(${state_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${state_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
