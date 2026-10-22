add_executable(test_lir_core test_lir_core.c)
add_executable(test_lir_c test_lir_c.c)
target_link_libraries(test_lir_c PRIVATE xray_lir_c)
add_dependencies(test_lir_c xray_native_runtime)
add_executable(lir_c_run lir_c_run.c)
target_link_libraries(lir_c_run PRIVATE xray_lir_c xray_lir_lower)
add_executable(test_lir_c_faults test_lir_c_faults.c "${PROJECT_SOURCE_DIR}/src/aot/xi_cgen_verify_output.c")
target_link_libraries(test_lir_c_faults PRIVATE xray_lir xray_compile_resources)
target_link_libraries(test_lir_core PRIVATE xray_lir)
add_executable(test_lir_source_input test_lir_source_input.c)
target_link_libraries(test_lir_source_input PRIVATE xray_xir_source)
add_executable(test_lir_lower test_lir_lower.c)
target_link_libraries(test_lir_lower PRIVATE xray_lir_lower)
add_executable(test_lir_lower_allocations test_lir_lower_allocations.c)
target_link_libraries(test_lir_lower_allocations PRIVATE xray_lir xray_xir_source)
add_executable(test_lir_lower_allocations_l3 test_lir_lower_allocations_l3.c)
target_link_libraries(test_lir_lower_allocations_l3 PRIVATE xray_lir xray_xir_source)
add_executable(test_lir_allocations test_lir_allocations.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
add_executable(test_lir_allocations_l3 test_lir_allocations_l3.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
add_executable(test_lir_print test_lir_print.c)
target_link_libraries(test_lir_print PRIVATE xray_lir)
add_executable(test_lir_print_allocations test_lir_print_allocations.c)
target_link_libraries(test_lir_print_allocations PRIVATE xray_lir)
foreach(target lir_c_run test_lir_c test_lir_c_faults test_lir_core test_lir_source_input test_lir_lower test_lir_lower_allocations test_lir_lower_allocations_l3
        test_lir_allocations test_lir_allocations_l3 test_lir_print test_lir_print_allocations)
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
add_test(NAME test_lir_core COMMAND test_lir_core)
add_test(NAME test_lir_c COMMAND test_lir_c)
add_test(NAME test_lir_c_faults COMMAND test_lir_c_faults)
if(MSVC)
    set(lir_c_provider_kind msvc)
else()
    set(lir_c_provider_kind gnu)
endif()
set(lir_c_native_options)
if(ENABLE_ASAN)
    list(APPEND lir_c_native_options --asan)
endif()
add_test(NAME test_lir_c_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_native.py"
    --driver $<TARGET_FILE:test_lir_c> --faults $<TARGET_FILE:test_lir_c_faults>
    --source-driver $<TARGET_FILE:lir_c_run>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --work "${CMAKE_CURRENT_BINARY_DIR}/native" ${lir_c_native_options})
set_tests_properties(test_lir_c_native PROPERTIES LABELS "unit;lir;native" TIMEOUT 120)
add_test(NAME test_lir_c_runtime COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_lir_c_runtime.py"
    --driver $<TARGET_FILE:lir_c_run> --fixture-driver $<TARGET_FILE:test_lir_c>
    --compiler "${CMAKE_C_COMPILER}" --kind "${lir_c_provider_kind}"
    --include "${PROJECT_SOURCE_DIR}/include" --library $<TARGET_FILE:xray_native_runtime>
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work "${CMAKE_CURRENT_BINARY_DIR}/runtime" ${lir_c_native_options})
set_tests_properties(test_lir_c_runtime PROPERTIES LABELS "unit;lir;native" TIMEOUT 120)
add_test(NAME test_lir_allocations COMMAND test_lir_allocations)
add_test(NAME test_lir_source_input COMMAND test_lir_source_input)
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
