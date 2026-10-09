# Replace exactly the allocator-bearing production objects, never their owners.
add_library(registry_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/runtime/class/xtype_registry.c
    ${CMAKE_SOURCE_DIR}/src/base/xhashmap.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
add_dependencies(registry_allocation_probe xray_core_objs)
target_include_directories(registry_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(registry_allocation_probe PRIVATE
    XR_REGISTRY_ALLOCATION_PROBE_OBJECT=1
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_compile_options(registry_allocation_probe PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
if(MSVC)
    target_compile_options(registry_allocation_probe PRIVATE /W4 /WX /utf-8 /wd4200
        "/FI${CMAKE_CURRENT_LIST_DIR}/xr_registry_allocation_probe.h")
else()
    target_compile_options(registry_allocation_probe PRIVATE -Wall -Wextra -Werror
        -include "${CMAKE_CURRENT_LIST_DIR}/xr_registry_allocation_probe.h")
endif()
add_executable(test_registry_allocations
    ${CMAKE_CURRENT_LIST_DIR}/test_registry_allocations.c
    $<TARGET_OBJECTS:registry_allocation_probe>
    "$<FILTER:$<TARGET_OBJECTS:xray_core_objs>,EXCLUDE,/(xtype_registry|xhashmap|xio_policy)[.]c[.](o|obj)$>")
target_include_directories(test_registry_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(test_registry_allocations PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_link_libraries(test_registry_allocations PRIVATE xray_stdlib_bootstrap_core)
if(MSVC)
    target_compile_options(test_registry_allocations PRIVATE /W4 /WX /utf-8 /wd4200)
else()
    target_compile_options(test_registry_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_registry_allocations COMMAND test_registry_allocations)
set_tests_properties(test_registry_allocations PROPERTIES
    LABELS "unit;runtime;ownership;allocation-failure" RUN_SERIAL TRUE TIMEOUT 120)
