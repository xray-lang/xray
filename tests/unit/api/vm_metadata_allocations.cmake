# Keep the actual constructor, metadata owners, map and allocator policy.
add_library(vm_metadata_allocation_probe OBJECT
    ${CMAKE_SOURCE_DIR}/src/runtime/symbol/xsymbol_table.c
    ${CMAKE_SOURCE_DIR}/src/runtime/class/xtype_registry.c
    ${CMAKE_SOURCE_DIR}/src/base/xhashmap.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
add_library(vm_metadata_call_probe OBJECT ${CMAKE_SOURCE_DIR}/src/api/xisolate_full.c)
target_compile_definitions(vm_metadata_allocation_probe PRIVATE XR_VM_METADATA_ALLOCATION_PROBE_OBJECT=1)
target_compile_definitions(vm_metadata_call_probe PRIVATE XR_VM_METADATA_CALL_PROBE_OBJECT=1)
foreach(metadata_probe IN ITEMS vm_metadata_allocation_probe vm_metadata_call_probe)
    add_dependencies(${metadata_probe} xray_core_objs)
    target_include_directories(${metadata_probe} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_compile_definitions(${metadata_probe} PRIVATE $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
    target_compile_options(${metadata_probe} PRIVATE $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
    if(MSVC)
        target_compile_options(${metadata_probe} PRIVATE /W4 /WX /utf-8 /wd4200
            "/FI${CMAKE_CURRENT_LIST_DIR}/xr_vm_metadata_probe.h")
    else()
        target_compile_options(${metadata_probe} PRIVATE -Wall -Wextra -Werror
            -include "${CMAKE_CURRENT_LIST_DIR}/xr_vm_metadata_probe.h")
    endif()
endforeach()
add_executable(test_vm_metadata_allocations
    ${CMAKE_CURRENT_LIST_DIR}/test_vm_metadata_allocations.c
    $<TARGET_OBJECTS:vm_metadata_allocation_probe>
    $<TARGET_OBJECTS:vm_metadata_call_probe>
    "$<FILTER:$<TARGET_OBJECTS:xray_core_objs>,EXCLUDE,/(xsymbol_table|xtype_registry|xhashmap|xio_policy|xisolate_full)[.]c[.](o|obj)$>")
target_include_directories(test_vm_metadata_allocations PRIVATE ${XRAY_COMMON_INCLUDES})
target_compile_definitions(test_vm_metadata_allocations PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_link_libraries(test_vm_metadata_allocations PRIVATE xray_stdlib_bootstrap_core)
if(MSVC)
    target_compile_options(test_vm_metadata_allocations PRIVATE /W4 /WX /utf-8 /wd4200)
else()
    target_compile_options(test_vm_metadata_allocations PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_vm_metadata_allocations COMMAND test_vm_metadata_allocations)
set_tests_properties(test_vm_metadata_allocations PROPERTIES
    LABELS "unit;runtime;ownership;allocation-failure" RUN_SERIAL TRUE TIMEOUT 120)
