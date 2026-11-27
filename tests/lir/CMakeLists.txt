add_executable(test_lir_core test_lir_core.c)
add_executable(test_lir_assertion test_lir_assertion.c)
target_link_libraries(test_lir_assertion PRIVATE xray_lir_bytecode)
add_test(NAME lir_assertion COMMAND test_lir_assertion)
set_tests_properties(lir_assertion PROPERTIES LABELS "unit;lir;vm;ownership" TIMEOUT 30)
add_executable(test_lir_state test_lir_state.c)
add_executable(test_lir_state_projection test_lir_state_projection.c)
target_link_libraries(test_lir_state_projection PRIVATE xray_lir_value_layout)
add_test(NAME lir_state_projection COMMAND test_lir_state_projection)
set_tests_properties(lir_state_projection PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_executable(test_lir_value_layout test_lir_value_layout.c)
add_executable(test_lir_binding_homes test_lir_binding_homes.c)
target_link_libraries(test_lir_binding_homes PRIVATE xray_lir_value_layout)
add_test(NAME lir_binding_homes COMMAND test_lir_binding_homes)
set_tests_properties(lir_binding_homes PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
target_link_libraries(test_lir_value_layout PRIVATE xray_lir_value_layout)
add_test(NAME lir_value_layout COMMAND test_lir_value_layout)
set_tests_properties(lir_value_layout PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_executable(test_lir_value_image test_lir_value_image.c)
target_link_libraries(test_lir_value_image PRIVATE xray_lir_value_layout)
add_test(NAME lir_value_image COMMAND test_lir_value_image)
set_tests_properties(lir_value_image PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_executable(test_lir_array test_lir_array.c)
add_executable(test_lir_array_faults test_lir_array_faults.c)
add_executable(test_lir_ownership test_lir_ownership.c)
add_executable(test_lir_ownership_faults test_lir_ownership_faults.c)
target_link_libraries(test_lir_ownership PRIVATE xray_lir_value_layout)
target_link_libraries(test_lir_ownership_faults PRIVATE xray_lir_value_layout)
target_link_libraries(test_lir_array PRIVATE xray_lir_c xray_lir_bytecode)
target_link_libraries(test_lir_array_faults PRIVATE xray_lir)
target_link_libraries(test_lir_state PRIVATE xray_lir)
add_executable(test_lir_carriers test_lir_carriers.c)
target_link_libraries(test_lir_carriers PRIVATE xray_lir_c xray_lir_bytecode)
add_executable(test_lir_callable_bytecode test_lir_callable_bytecode.c)
target_link_libraries(test_lir_callable_bytecode PRIVATE xray_lir_bytecode)
add_test(NAME lir_callable_bytecode COMMAND test_lir_callable_bytecode)
set_tests_properties(lir_callable_bytecode PROPERTIES LABELS "unit;lir;vm" TIMEOUT 30)
add_executable(test_lir_callable_facts test_lir_callable_facts.c)
target_link_libraries(test_lir_callable_facts PRIVATE xray_lir_value_layout)
add_test(NAME lir_callable_facts COMMAND test_lir_callable_facts)
set_tests_properties(lir_callable_facts PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_executable(test_lir_inline test_lir_inline.c)
target_link_libraries(test_lir_inline PRIVATE xray_lir_c xray_lir_bytecode)
add_dependencies(test_lir_inline xray_native_runtime)
add_executable(test_lir_inline_faults test_lir_inline_faults.c)
target_link_libraries(test_lir_inline_faults PRIVATE xray_lir)
add_executable(test_lir_sccp test_lir_sccp.c)
target_link_libraries(test_lir_sccp PRIVATE xray_lir_c xray_lir_bytecode)
add_dependencies(test_lir_sccp xray_native_runtime)
add_executable(test_lir_sccp_faults test_lir_sccp_faults.c)
target_link_libraries(test_lir_sccp_faults PRIVATE xray_lir)
add_executable(test_lir_rebuild test_lir_rebuild.c)
target_link_libraries(test_lir_rebuild PRIVATE xray_lir_c xray_lir_bytecode)
add_executable(test_lir_rebuild_faults test_lir_rebuild_faults.c)
target_link_libraries(test_lir_rebuild_faults PRIVATE xray_lir)
add_executable(test_lir_string_elide test_lir_string_elide.c)
target_link_libraries(test_lir_string_elide PRIVATE xray_lir xray_lir_bytecode)
add_executable(test_lir_c test_lir_c.c)
add_executable(test_lir_c_value_types test_lir_c_value_types.c)
target_link_libraries(test_lir_c_value_types PRIVATE xray_lir_c)
add_executable(test_lir_c_value_transfers test_lir_c_value_transfers.c)
target_link_libraries(test_lir_c_value_transfers PRIVATE xray_lir_c)
target_link_libraries(test_lir_c PRIVATE xray_lir_c)
add_dependencies(test_lir_c xray_native_runtime)
add_executable(lir_c_run lir_c_run.c)
target_link_libraries(lir_c_run PRIVATE xray_lir_c xray_lir_lower)
add_executable(lir_test_suite_vm lir_test_suite_vm.c)
target_link_libraries(lir_test_suite_vm PRIVATE xray_lir_c xray_lir_lower xray_lir_bytecode xray_native_runtime)
add_executable(lir_pipeline_eval lir_pipeline_eval.c)
target_link_libraries(lir_pipeline_eval PRIVATE xray_lir_c xray_lir_lower xray_lir_bytecode xray_native_runtime)
add_executable(lir_host_stat_control lir_host_stat_control.c)
target_link_libraries(lir_host_stat_control PRIVATE xray_lir_lower xray_lir_bytecode xray_native_runtime)
add_dependencies(lir_host_stat_control xray)
add_executable(lir_host_remove_control lir_host_remove_control.c)
target_link_libraries(lir_host_remove_control PRIVATE xray_lir_lower xray_lir_bytecode xray_native_runtime)
add_dependencies(lir_host_remove_control xray)
add_executable(test_lir_c_faults test_lir_c_faults.c "${PROJECT_SOURCE_DIR}/src/lir/xi_cgen_verify_output.c")
target_include_directories(test_lir_c_faults PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_lir_c_faults PRIVATE xray_lir_value_layout xray_compile_resources)
target_link_libraries(test_lir_core PRIVATE xray_lir)
add_executable(test_lir_source_input test_lir_source_input.c)
target_link_libraries(test_lir_source_input PRIVATE xray_xir_source)
add_executable(test_lir_lower test_lir_lower.c)
target_link_libraries(test_lir_lower PRIVATE xray_lir_lower xray_lir_value_layout)
add_executable(test_lir_lower_allocations test_lir_lower_allocations.c)
target_link_libraries(test_lir_lower_allocations PRIVATE xray_lir xray_xir_source)
add_executable(test_lir_lower_allocations_l3 test_lir_lower_allocations_l3.c)
target_link_libraries(test_lir_lower_allocations_l3 PRIVATE xray_lir xray_xir_source)
add_executable(test_lir_allocations test_lir_allocations.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
add_executable(test_lir_allocations_l3 test_lir_allocations_l3.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
add_executable(test_lir_block_facts_faults test_lir_block_facts_faults.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir.c")
add_executable(test_lir_print test_lir_print.c)
target_link_libraries(test_lir_print PRIVATE xray_lir)
add_executable(test_lir_print_allocations test_lir_print_allocations.c)
target_link_libraries(test_lir_print_allocations PRIVATE xray_lir)
foreach(target test_lir_assertion test_lir_binding_homes test_lir_callable_bytecode test_lir_callable_facts test_lir_carriers test_lir_inline test_lir_inline_faults test_lir_sccp test_lir_sccp_faults test_lir_rebuild test_lir_rebuild_faults test_lir_string_elide lir_c_run lir_pipeline_eval test_lir_c test_lir_c_faults test_lir_core test_lir_source_input test_lir_lower test_lir_lower_allocations test_lir_lower_allocations_l3
        test_lir_allocations test_lir_allocations_l3 test_lir_block_facts_faults test_lir_print test_lir_print_allocations test_lir_state test_lir_state_projection test_lir_value_layout test_lir_value_image test_lir_array test_lir_array_faults
        test_lir_ownership test_lir_ownership_faults test_lir_c_value_types test_lir_c_value_transfers lir_host_stat_control lir_host_remove_control lir_test_suite_vm)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
# These failure-injection units compile the owning implementation directly.
foreach(target test_lir_allocations test_lir_allocations_l3 test_lir_block_facts_faults test_lir_array_faults test_lir_rebuild_faults)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/include")
endforeach()
add_test(NAME test_lir_core COMMAND test_lir_core)
add_test(NAME test_lir_state COMMAND test_lir_state)
add_test(NAME test_lir_array COMMAND test_lir_array)
add_test(NAME test_lir_array_faults COMMAND test_lir_array_faults)
add_test(NAME test_lir_ownership COMMAND test_lir_ownership)
add_test(NAME test_lir_ownership_faults COMMAND test_lir_ownership_faults)
set_tests_properties(test_lir_ownership test_lir_ownership_faults PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
set_tests_properties(test_lir_array test_lir_array_faults PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_test(NAME test_lir_carriers COMMAND test_lir_carriers)
add_test(NAME test_lir_inline COMMAND test_lir_inline)
add_test(NAME test_lir_inline_faults COMMAND test_lir_inline_faults)
add_test(NAME test_lir_rebuild COMMAND test_lir_rebuild)
add_test(NAME test_lir_rebuild_faults COMMAND test_lir_rebuild_faults)
add_test(NAME test_lir_string_elide COMMAND test_lir_string_elide)
add_test(NAME test_lir_sccp COMMAND test_lir_sccp)
add_test(NAME test_lir_sccp_faults COMMAND test_lir_sccp_faults)
add_test(NAME test_lir_c COMMAND test_lir_c)
add_test(NAME test_lir_c_faults COMMAND test_lir_c_faults)
add_test(NAME lir_c_value_types COMMAND test_lir_c_value_types)
set_tests_properties(lir_c_value_types PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
add_test(NAME lir_c_value_transfers COMMAND test_lir_c_value_transfers)
set_tests_properties(lir_c_value_transfers PROPERTIES LABELS "unit;lir;ownership" TIMEOUT 30)
if(MSVC)
    set(lir_c_provider_kind msvc)
else()
    set(lir_c_provider_kind gnu)
endif()
set(lir_c_native_options)
if(ENABLE_ASAN)
    list(APPEND lir_c_native_options --asan)
endif()
set(lir_c_transfer_options ${lir_c_native_options})
if(ENABLE_UBSAN)
    list(APPEND lir_c_transfer_options --ubsan)
endif()
if(ENABLE_ASAN AND CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_LINKER MATCHES "lld-link")
    list(APPEND lir_c_transfer_options --lld-no-tail-merge)
endif()
if(DEFINED _XRAY_WINDOWS_UBSAN_RUNTIME AND NOT ENABLE_ASAN)
    list(APPEND lir_c_transfer_options --ubsan-runtime "${_XRAY_WINDOWS_UBSAN_RUNTIME}")
endif()
add_test(NAME lir_c_value_transfers_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_value_transfers_native.py"
    --driver $<TARGET_FILE:test_lir_c_value_transfers>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/value-transfers" ${lir_c_transfer_options})
set_tests_properties(lir_c_value_transfers_native PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 30)
add_test(NAME lir_c_managed_strings_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_managed_strings_native.py"
    --driver $<TARGET_FILE:lir_c_run> --faults $<TARGET_FILE:test_lir_c_faults>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/managed-strings" ${lir_c_transfer_options})
set_tests_properties(lir_c_managed_strings_native PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 30)
add_test(NAME lir_c_value_types_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_value_types_native.py"
    --driver $<TARGET_FILE:test_lir_c_value_types>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --work "${CMAKE_CURRENT_BINARY_DIR}/value-types")
set_tests_properties(lir_c_value_types_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 30)
set(lir_c_native_command ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_native.py")
if(TARGET xray_stdlib_scalar_export)
    set(stdlib_export_sanitizers)
    set(stdlib_export_interface)
    if(XRAY_STDLIB_CHECKED_INTERFACE_C)
        list(APPEND stdlib_export_interface --interface "${XRAY_STDLIB_CHECKED_INTERFACE_C}")
    endif()
    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        list(APPEND stdlib_export_sanitizers --asan)
    endif()
    if(ENABLE_UBSAN OR ENABLE_SANITIZERS)
        list(APPEND stdlib_export_sanitizers --ubsan)
    endif()
    set(lir_c_native_command ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_stdlib_scalar_export.py"
        --producer $<TARGET_FILE:xray_stdlib_scalar_producer>
        --xray $<TARGET_FILE:xray>
        --archive $<TARGET_FILE:xray_stdlib_scalar_export>
        --generated "${XRAY_STDLIB_SCALAR_EXPORT_C}"
        ${stdlib_export_interface} --internal-include "${PROJECT_SOURCE_DIR}/src"
        --stdlib "${PROJECT_SOURCE_DIR}/stdlib"
        --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
        --msvc-runtime "$<TARGET_GENEX_EVAL:xray_stdlib_scalar_export,$<TARGET_PROPERTY:xray_stdlib_scalar_export,MSVC_RUNTIME_LIBRARY>>"
        --work "${CMAKE_CURRENT_BINARY_DIR}/native" ${stdlib_export_sanitizers}
        -- ${lir_c_native_command})
endif()
add_test(NAME test_lir_c_native COMMAND ${lir_c_native_command}
    --driver $<TARGET_FILE:test_lir_c> --faults $<TARGET_FILE:test_lir_c_faults>
    --source-driver $<TARGET_FILE:lir_c_run> --binary $<TARGET_FILE:xray>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --work "${CMAKE_CURRENT_BINARY_DIR}/native" ${lir_c_native_options})
set_tests_properties(test_lir_c_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 120)
set(lir_array_native_options ${lir_c_native_options})
if(ENABLE_ASAN AND NOT MSVC AND CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$")
    # Keep symbolized sanitizer locations without full debug records for spill fixtures.
    list(APPEND lir_array_native_options --line-tables-only)
endif()
set(lir_array_native_processors 1)
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "MSVC" AND CMAKE_BUILD_TYPE STREQUAL "Release"
   AND NOT (ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN OR ENABLE_SANITIZERS))
    set(lir_array_native_processors 2)
    list(APPEND lir_array_native_options --jobs 2)
endif()
add_test(NAME test_lir_array_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_array_native.py"
    --driver $<TARGET_FILE:test_lir_array> --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/array-native" ${lir_array_native_options})
set_tests_properties(test_lir_array_native PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 120 PROCESSORS ${lir_array_native_processors})
add_test(NAME test_lir_c_runtime COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_runtime.py"
    --driver $<TARGET_FILE:lir_c_run> --fixture-driver $<TARGET_FILE:test_lir_c>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/runtime" ${lir_c_native_options})
set_tests_properties(test_lir_c_runtime PROPERTIES LABELS "unit;lir;native" TIMEOUT 120)
add_test(NAME test_lir_divrem COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_divrem.py"
    --driver $<TARGET_FILE:lir_c_run> --vm-driver $<TARGET_FILE:lir_vm_run> --fixture-driver $<TARGET_FILE:test_lir_c>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/divrem" ${lir_c_native_options})
set_tests_properties(test_lir_divrem PROPERTIES LABELS "unit;lir;native" TIMEOUT 120)
add_test(NAME test_lir_test_role_startup COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_test_role_startup.py"
    --driver $<TARGET_FILE:lir_c_run> --vm-driver $<TARGET_FILE:lir_vm_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/test-role" ${lir_c_native_options})
set_tests_properties(test_lir_test_role_startup PROPERTIES LABELS "unit;lir;vm;native;test-role" TIMEOUT 120)
add_test(NAME test_lir_test_suite_catalog COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_test_suite_catalog.py"
    --vm-driver $<TARGET_FILE:lir_test_suite_vm>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/test-suite" ${lir_c_native_options})
set_tests_properties(test_lir_test_suite_catalog PROPERTIES LABELS "unit;lir;vm;native;test-role" TIMEOUT 120)
add_test(NAME test_lir_owned_string COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_owned_string.py"
    --driver $<TARGET_FILE:lir_pipeline_eval> --source-driver $<TARGET_FILE:lir_c_run>
    --result-driver $<TARGET_FILE:test_lir_bytecode_source>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/owned-string" ${lir_c_native_options})
set_tests_properties(test_lir_owned_string PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 180)
add_test(NAME lir_class_union_is COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_class_union_is.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/class-union-is" ${lir_c_native_options})
set_tests_properties(lir_class_union_is PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_sum_types COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/sum-types.xr --expect "0,1,2,9,16,0,25,0"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-types" ${lir_c_native_options})
set_tests_properties(lir_sum_types PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_sum_solo COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/sum-solo.xr --expect "41,42"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-solo" ${lir_c_native_options})
set_tests_properties(lir_sum_solo PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_sum_layout COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/sum-layout.xr --expect "7,8,9,10,23,26,-1"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-layout" ${lir_c_native_options})
set_tests_properties(lir_sum_layout PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_sum_carrier_gate COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/sum-carrier-gate.xr
    --refuse "enum payload field type is not supported by LIR lowering yet"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-carrier-gate" ${lir_c_native_options})
set_tests_properties(lir_sum_carrier_gate PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_sum_tag_width COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_tag_width.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --variants 257
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-tag-width" ${lir_c_native_options})
set_tests_properties(lir_sum_tag_width PROPERTIES LABELS "unit;lir;native" TIMEOUT 300)
# The four-byte tag needs more than 65536 variants, which is a slow program in a
# Debug sanitizer build, so it gets its own lane with a matching command budget.
add_test(NAME lir_sum_tag_width_wide COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_tag_width.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --variants 65537 --command-timeout 1800
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/sum-tag-width-wide" ${lir_c_native_options})
set_tests_properties(lir_sum_tag_width_wide PROPERTIES LABELS "unit;lir;native" TIMEOUT 3600)
# A data union is a compiler-synthesized enum: same-member-set identity,
# same-family members, aliases, and the `is`/`match` variant tests. Every case
# runs through the register VM and the generated native C program.
add_test(NAME lir_union_same_family COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/union-same-family.xr --expect "i16,i32,f32,f64,1.5,1.5,2.5,0.0"
    --c-contains "UINT64_C(1069547520),UINT64_C(1075838976)" --c-absent "(float)UINT64_C(0)"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/union-same-family" ${lir_c_native_options})
set_tests_properties(lir_union_same_family PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_union_i8_i64 COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/union-i8-i64.xr --expect "i8,true,false,i64,false,true"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/union-i8-i64" ${lir_c_native_options})
set_tests_properties(lir_union_i8_i64 PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_union_identity COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/union-identity.xr --expect "i16,i16,i16 | i32,i16 | i32"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/union-identity" ${lir_c_native_options})
set_tests_properties(lir_union_identity PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME lir_union_alias COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sum_types.py"
    --vm-driver $<TARGET_FILE:lir_vm_run> --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --program tests/lir/programs/union-alias.xr --expect "i16,i16,i16 | i32"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/union-alias" ${lir_c_native_options})
set_tests_properties(lir_union_alias PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME test_lir_pipeline_eval COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_pipeline_eval.py"
    --driver $<TARGET_FILE:lir_pipeline_eval>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --helper "${CMAKE_CURRENT_SOURCE_DIR}/programs/source-eval-helper.xr"
    --work "${CMAKE_CURRENT_BINARY_DIR}/pipeline" ${lir_c_native_options})
set_tests_properties(test_lir_pipeline_eval PROPERTIES LABELS "unit;lir;native;readiness" TIMEOUT 180)
add_test(NAME test_lir_host_stat_control COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_host_stat_control.py"
    --binary $<TARGET_FILE:xray> --driver $<TARGET_FILE:lir_host_stat_control>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/host-stat-control" ${lir_c_transfer_options})
set_tests_properties(test_lir_host_stat_control PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 120)
add_test(NAME test_lir_host_remove_control COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_host_remove_control.py"
    --binary $<TARGET_FILE:xray> --driver $<TARGET_FILE:lir_host_remove_control>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --source-root "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/host-remove-control" ${lir_c_transfer_options})
set_tests_properties(test_lir_host_remove_control PROPERTIES LABELS "unit;lir;native;ownership" TIMEOUT 120)
add_test(NAME test_lir_rebuild_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_rebuild_native.py"
    --driver $<TARGET_FILE:test_lir_rebuild>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/rebuild" ${lir_c_native_options})
set_tests_properties(test_lir_rebuild_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME test_lir_sccp_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_sccp_native.py"
    --driver $<TARGET_FILE:test_lir_sccp>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/sccp" ${lir_c_native_options})
set_tests_properties(test_lir_sccp_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME test_lir_inline_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_inline_native.py"
    --driver $<TARGET_FILE:test_lir_inline>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/inline" ${lir_c_native_options})
set_tests_properties(test_lir_inline_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
add_test(NAME test_lir_carriers_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_carriers_native.py"
    --driver $<TARGET_FILE:test_lir_carriers>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/carriers" ${lir_c_native_options})
set_tests_properties(test_lir_carriers_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 180)
set_tests_properties(test_lir_carriers PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
set_tests_properties(test_lir_inline test_lir_inline_faults PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
set_tests_properties(test_lir_sccp test_lir_sccp_faults PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
set_tests_properties(test_lir_rebuild test_lir_rebuild_faults test_lir_string_elide PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
add_test(NAME test_lir_allocations COMMAND test_lir_allocations)
add_test(NAME lir_block_facts_faults COMMAND test_lir_block_facts_faults)
set_tests_properties(lir_block_facts_faults PROPERTIES LABELS "unit;lir;allocation;ownership" TIMEOUT 30)
add_test(NAME test_lir_source_input COMMAND test_lir_source_input "${PROJECT_SOURCE_DIR}/stdlib")
add_test(NAME test_lir_lower COMMAND test_lir_lower "${CMAKE_CURRENT_SOURCE_DIR}/programs" "${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME test_lir_lower_allocations COMMAND test_lir_lower_allocations)
add_test(NAME test_lir_print COMMAND test_lir_print "${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME test_lir_print_allocations COMMAND test_lir_print_allocations)
add_test(NAME l3_lir_allocations COMMAND test_lir_allocations_l3)
add_test(NAME l3_lir_lower_allocations COMMAND test_lir_lower_allocations_l3)
set_tests_properties(l3_lir_allocations l3_lir_lower_allocations PROPERTIES LABELS "l3;lir;allocation" TIMEOUT 30)
add_test(NAME test_lir_ownership_inventory_scope
    COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_inventory_scope.py"
        "${PROJECT_SOURCE_DIR}/scripts/check_meta_ownership.py" "${CMAKE_CURRENT_BINARY_DIR}/scratch")
set_tests_properties(test_lir_c test_lir_c_faults test_lir_core test_lir_allocations test_lir_source_input test_lir_lower
    test_lir_lower_allocations test_lir_ownership_inventory_scope test_lir_print test_lir_print_allocations
    PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
