# Replace exactly the allocator-bearing production objects, never their owners.
add_library(sysheap_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/runtime/mem/xsystem_heap.c
    ${CMAKE_SOURCE_DIR}/src/coro/xcoro_pool.c
    ${CMAKE_SOURCE_DIR}/src/base/xarena_backing.c)
add_dependencies(sysheap_allocation_probe xray_core_objs)
target_include_directories(sysheap_allocation_probe PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(sysheap_allocation_probe PRIVATE
    XR_SYSHEAP_ALLOCATION_PROBE_OBJECT=1
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_compile_options(sysheap_allocation_probe PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
if(MSVC)
    target_compile_options(sysheap_allocation_probe PRIVATE /W4 /WX /utf-8 /wd4200
        "/FI${CMAKE_CURRENT_LIST_DIR}/xr_sysheap_allocation_probe.h")
else()
    target_compile_options(sysheap_allocation_probe PRIVATE -Wall -Wextra -Werror
        -include "${CMAKE_CURRENT_LIST_DIR}/xr_sysheap_allocation_probe.h")
endif()
add_executable(test_sysheap_allocations
    ${CMAKE_CURRENT_LIST_DIR}/test_sysheap_allocations.c
    $<TARGET_OBJECTS:sysheap_allocation_probe>
    "$<FILTER:$<TARGET_OBJECTS:xray_core_objs>,EXCLUDE,/(xsystem_heap|xcoro_pool|xarena_backing)[.]c[.](o|obj)$>")
target_include_directories(test_sysheap_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(test_sysheap_allocations PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_link_libraries(test_sysheap_allocations PRIVATE xray_stdlib_bootstrap_core)
if(MSVC)
    target_compile_options(test_sysheap_allocations PRIVATE /W4 /WX /utf-8 /wd4200)
else()
    target_compile_options(test_sysheap_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_sysheap_allocations COMMAND test_sysheap_allocations)
set_tests_properties(test_sysheap_allocations PROPERTIES
    LABELS "unit;runtime;ownership;allocation-failure" RUN_SERIAL TRUE TIMEOUT 120)
