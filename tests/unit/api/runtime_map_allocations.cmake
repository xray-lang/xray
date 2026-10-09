# Recompile actual allocator owners and constructor call sites with observation.
add_library(runtime_map_policy_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/base/xhashmap.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
add_library(runtime_map_global_probe OBJECT ${CMAKE_SOURCE_DIR}/src/api/xglobal_object.c)
add_library(runtime_map_engine_probe OBJECT ${CMAKE_SOURCE_DIR}/src/api/xvm_exec.c)
add_library(runtime_map_call_probe OBJECT ${CMAKE_SOURCE_DIR}/src/api/xisolate_full.c)
target_compile_definitions(runtime_map_policy_probe PRIVATE XR_RUNTIME_MAP_ALLOCATION_PROBE=1)
target_compile_definitions(runtime_map_global_probe PRIVATE
    XR_RUNTIME_MAP_ALLOCATION_PROBE=1 XR_RUNTIME_MAP_GLOBAL_PROBE=1)
target_compile_definitions(runtime_map_engine_probe PRIVATE
    XR_RUNTIME_MAP_ALLOCATION_PROBE=1 XR_RUNTIME_MAP_ENGINE_PROBE=1)
target_compile_definitions(runtime_map_call_probe PRIVATE XR_RUNTIME_MAP_CALL_PROBE=1)
foreach(map_probe IN ITEMS runtime_map_policy_probe runtime_map_global_probe
        runtime_map_engine_probe runtime_map_call_probe)
    add_dependencies(${map_probe} xray_core_objs)
    target_include_directories(${map_probe} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_compile_definitions(${map_probe} PRIVATE $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
    target_compile_options(${map_probe} PRIVATE $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
    if(MSVC)
        target_compile_options(${map_probe} PRIVATE /W4 /WX /utf-8 /wd4200
            "/FI${CMAKE_CURRENT_LIST_DIR}/xr_runtime_map_probe.h")
    else()
        target_compile_options(${map_probe} PRIVATE -Wall -Wextra -Werror
            -include "${CMAKE_CURRENT_LIST_DIR}/xr_runtime_map_probe.h")
    endif()
endforeach()
add_executable(test_runtime_map_allocations
    ${CMAKE_CURRENT_LIST_DIR}/test_runtime_map_allocations.c
    $<TARGET_OBJECTS:runtime_map_policy_probe>
    $<TARGET_OBJECTS:runtime_map_global_probe>
    $<TARGET_OBJECTS:runtime_map_engine_probe>
    $<TARGET_OBJECTS:runtime_map_call_probe>
    "$<FILTER:$<TARGET_OBJECTS:xray_core_objs>,EXCLUDE,/(xglobal_object|xvm_exec|xhashmap|xio_policy|xisolate_full)[.]c[.](o|obj)$>")
target_include_directories(test_runtime_map_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(test_runtime_map_allocations PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_link_libraries(test_runtime_map_allocations PRIVATE xray_stdlib_bootstrap_core)
if(MSVC)
    target_compile_options(test_runtime_map_allocations PRIVATE /W4 /WX /utf-8 /wd4200)
else()
    target_compile_options(test_runtime_map_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_runtime_map_allocations COMMAND test_runtime_map_allocations)
set_tests_properties(test_runtime_map_allocations PROPERTIES
    LABELS "unit;runtime;ownership;allocation-failure" RUN_SERIAL TRUE TIMEOUT 120)
