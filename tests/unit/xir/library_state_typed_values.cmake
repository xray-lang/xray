# Both Source and Checked Catalog consumers execute independent state oracles.
set(XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_source.xrc)
set(XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_catalog.xrc)
add_library(xir_library_state_typed_values_source_runtime OBJECT xir/test_xir_library_state_typed_values_execution.c)
target_compile_definitions(xir_library_state_typed_values_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_library_state_typed_values_source_runtime PRIVATE xray_xir_vm)

file(GLOB XIR_LIBRARY_STATE_TYPED_VALUES_FIXTURES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_library_state_typed_values/*.xr")

set(XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS xir_library_state_typed_values_source_runtime)
foreach(state_flow IN ITEMS source catalog)
    if(state_flow STREQUAL "source")
        set(state_checked ${XIR_LIBRARY_STATE_TYPED_VALUES_SOURCE})
    else()
        set(state_checked ${XIR_LIBRARY_STATE_TYPED_VALUES_CATALOG})
    endif()
    set(state_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_state_typed_values_${state_flow}.c)

    foreach(state_mode IN ITEMS native mixed)
        set(state_target test_xir_library_state_typed_values_${state_flow}_${state_mode})

        list(APPEND XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS)

    endforeach()

endforeach()
foreach(state_target IN LISTS XIR_LIBRARY_STATE_TYPED_VALUES_TARGETS)
    if(MSVC)
        target_compile_options(${state_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${state_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
