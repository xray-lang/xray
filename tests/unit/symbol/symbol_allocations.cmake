# Replace exactly the allocator-bearing production objects, never their owners.
add_library(symbol_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/runtime/symbol/xsymbol_table.c
    ${CMAKE_SOURCE_DIR}/src/base/xhashmap.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
add_dependencies(symbol_allocation_probe xray_core_objs)
target_include_directories(symbol_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(symbol_allocation_probe PRIVATE
    XR_SYMBOL_ALLOCATION_PROBE_OBJECT=1
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_compile_options(symbol_allocation_probe PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
if(MSVC)
    target_compile_options(symbol_allocation_probe PRIVATE /W4 /WX /utf-8 /wd4200
        "/FI${CMAKE_CURRENT_LIST_DIR}/xr_symbol_allocation_probe.h")
else()
    target_compile_options(symbol_allocation_probe PRIVATE -Wall -Wextra -Werror
        -include "${CMAKE_CURRENT_LIST_DIR}/xr_symbol_allocation_probe.h")
endif()
add_executable(test_symbol_allocations
    ${CMAKE_CURRENT_LIST_DIR}/test_symbol_allocations.c
    $<TARGET_OBJECTS:symbol_allocation_probe>
    "$<FILTER:$<TARGET_OBJECTS:xray_core_objs>,EXCLUDE,/(xsymbol_table|xhashmap|xio_policy)[.]c[.](o|obj)$>")
target_include_directories(test_symbol_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(test_symbol_allocations PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_link_libraries(test_symbol_allocations PRIVATE xray_stdlib_bootstrap_core)
if(MSVC)
    target_compile_options(test_symbol_allocations PRIVATE /W4 /WX /utf-8 /wd4200)
else()
    target_compile_options(test_symbol_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_symbol_allocations COMMAND test_symbol_allocations)
set_tests_properties(test_symbol_allocations PROPERTIES
    LABELS "unit;runtime;ownership;allocation-failure" RUN_SERIAL TRUE TIMEOUT 120)
