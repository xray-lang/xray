# Both Source and Checked Catalog consumers execute independent state oracles.
set(XIR_UNIT_DESTRUCTURE_CELL_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_source.xrc)
set(XIR_UNIT_DESTRUCTURE_CELL_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_catalog.xrc)
set(XIR_UNIT_DESTRUCTURE_CELL_PENDING ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_pending.xrc)
add_library(xir_unit_destructure_cell_source_runtime OBJECT xir/test_xir_unit_destructure_cell_execution.c)
target_compile_definitions(xir_unit_destructure_cell_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_unit_destructure_cell_source_runtime PRIVATE xray_xir_vm)
add_executable(test_xir_unit_destructure_cell_source xir/test_xir_unit_destructure_cell_source.c
    $<TARGET_OBJECTS:xir_unit_destructure_cell_source_runtime>)
target_link_libraries(test_xir_unit_destructure_cell_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_unit_destructure_cell_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_destructure_cell")
file(GLOB XIR_UNIT_DESTRUCTURE_CELL_FIXTURES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_unit_destructure_cell/*.xr")
add_custom_command(OUTPUT ${XIR_UNIT_DESTRUCTURE_CELL_SOURCE} ${XIR_UNIT_DESTRUCTURE_CELL_CATALOG} ${XIR_UNIT_DESTRUCTURE_CELL_PENDING}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_unit_destructure_cell_source> --write-checked ${XIR_UNIT_DESTRUCTURE_CELL_SOURCE} ${XIR_UNIT_DESTRUCTURE_CELL_CATALOG} ${XIR_UNIT_DESTRUCTURE_CELL_PENDING}
    DEPENDS test_xir_unit_destructure_cell_source ${XIR_UNIT_DESTRUCTURE_CELL_FIXTURES}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_unit_destructure_cell_runtime.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_unit_destructure_cell_source_cases.h
    VERBATIM)
add_executable(test_xir_unit_destructure_cell_packet xir/test_xir_unit_destructure_cell_execution.c)
target_compile_definitions(test_xir_unit_destructure_cell_packet PRIVATE CONSUMER_KIND=0)
target_link_libraries(test_xir_unit_destructure_cell_packet PRIVATE xray_xir_vm xray_xir_cgen)
set(XIR_UNIT_DESTRUCTURE_CELL_TARGETS xir_unit_destructure_cell_source_runtime test_xir_unit_destructure_cell_source test_xir_unit_destructure_cell_packet)
foreach(state_flow IN ITEMS source catalog pending)
    if(state_flow STREQUAL "source")
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_SOURCE})
    elseif(state_flow STREQUAL "catalog")
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_CATALOG})
    else()
        set(state_checked ${XIR_UNIT_DESTRUCTURE_CELL_PENDING})
    endif()
    set(state_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_unit_destructure_cell_${state_flow}.c)
    add_custom_command(OUTPUT ${state_generated}
        COMMAND $<TARGET_FILE:test_xir_unit_destructure_cell_packet> --write-c ${state_checked} ${state_generated}
        DEPENDS test_xir_unit_destructure_cell_packet ${state_checked}
        VERBATIM)
    set(state_pending_arg)
    if(state_flow STREQUAL "pending")
        set(state_pending_arg --pending)
    endif()
    add_test(NAME test_xir_unit_destructure_cell_${state_flow}_packet COMMAND test_xir_unit_destructure_cell_packet
        ${state_checked} ${CMAKE_CURRENT_BINARY_DIR}/unit-destructure-${state_flow}-packet-test.c ${state_pending_arg})
    foreach(state_mode IN ITEMS native mixed)
        set(state_target test_xir_unit_destructure_cell_${state_flow}_${state_mode})
        add_executable(${state_target} xir/test_xir_unit_destructure_cell_execution.c ${state_generated})
        target_link_libraries(${state_target} PRIVATE xray_xir_vm)
        if(state_mode STREQUAL "mixed")
            target_compile_definitions(${state_target} PRIVATE CONSUMER_KIND=2)
        else()
            target_compile_definitions(${state_target} PRIVATE CONSUMER_KIND=1)
        endif()
        if(state_flow STREQUAL "pending")
            target_compile_definitions(${state_target} PRIVATE UNIT_PENDING=1)
        endif()
        list(APPEND XIR_UNIT_DESTRUCTURE_CELL_TARGETS ${state_target})
        add_test(NAME ${state_target} COMMAND ${state_target})
    endforeach()
    set_tests_properties(test_xir_unit_destructure_cell_${state_flow}_packet
        test_xir_unit_destructure_cell_${state_flow}_native test_xir_unit_destructure_cell_${state_flow}_mixed
        PROPERTIES LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()
foreach(state_target IN LISTS XIR_UNIT_DESTRUCTURE_CELL_TARGETS)
    if(MSVC)
        target_compile_options(${state_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${state_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_unit_destructure_cell_source COMMAND test_xir_unit_destructure_cell_source)
set_tests_properties(test_xir_unit_destructure_cell_source PROPERTIES
    LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 1200 RUN_SERIAL TRUE COST 120)
