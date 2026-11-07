

xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()

# Complete evaluator fault jobs measured near the original30-second ASan cap.
# Serialize these real receiving/verification jobs without changing any limit.

# Measured sanitizer cost puts the largest serial fault job early on cold schedulers.

# Complete fault jobs retain their measured serial sanitizer scheduling costs.

add_executable(test_xir_ctfe_slice_u8 "${CMAKE_CURRENT_LIST_DIR}/test_ctfe_slice_u8.c")
target_link_libraries(test_xir_ctfe_slice_u8 PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_ctfe_slice_u8 PRIVATE XR_CTFE_SLICE_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_ctfe_slice_u8 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_slice_u8 PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_ctfe_slice_u8 PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(name IN ITEMS slice_u8_local slice_u8_string slice_u8_arguments)
    add_test(NAME test_xir_ctfe_${name} COMMAND test_xir_ctfe_slice_u8 ${name})
    set_tests_properties(test_xir_ctfe_${name} PROPERTIES LABELS "unit;xir;ctfe;ownership" TIMEOUT 60)
endforeach()

foreach(mode IN ITEMS resources authority controls boundaries memory runtime-work argument-resources argument-authority argument-cancel argument-memory)
    add_test(NAME test_xir_ctfe_slice_u8_${mode} COMMAND test_xir_ctfe_slice_u8 ${mode})
    set_tests_properties(test_xir_ctfe_slice_u8_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;resources" TIMEOUT 60)
endforeach()

set(ctfe_from_utf8_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/from_utf8.xr")
set(ctfe_from_utf8_stdout "${CMAKE_CURRENT_LIST_DIR}/fixtures/from_utf8.stdout")
set(ctfe_from_utf8_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/ctfe_from_utf8.c")
add_custom_command(OUTPUT "${ctfe_from_utf8_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${ctfe_from_utf8_fixture}" "${ctfe_from_utf8_generated}"
    DEPENDS test_xir_from_utf8 "${ctfe_from_utf8_fixture}" VERBATIM)
add_executable(test_xir_ctfe_from_utf8_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${ctfe_from_utf8_generated}")
target_link_libraries(test_xir_ctfe_from_utf8_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_ctfe_from_utf8_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_ctfe_from_utf8_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_ctfe_from_utf8
    COMMAND test_xir_from_utf8 --vm-file "${ctfe_from_utf8_fixture}" --expect "${ctfe_from_utf8_stdout}")
add_test(NAME test_xir_ctfe_from_utf8_native
    COMMAND test_xir_ctfe_from_utf8_native --expect "${ctfe_from_utf8_stdout}")
set_tests_properties(test_xir_ctfe_from_utf8 test_xir_ctfe_from_utf8_native PROPERTIES
    LABELS "unit;xir;ctfe;source;ownership;fast" TIMEOUT 60)

target_compile_definitions(test_xir_ctfe_slice_u8 PRIVATE XR_CTFE_UTF8_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
foreach(mode oom control lifetime types memory packet)
    add_test(NAME test_xir_ctfe_from_utf8_${mode} COMMAND test_xir_ctfe_slice_u8 native-${mode})
    set_tests_properties(test_xir_ctfe_from_utf8_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;resources" TIMEOUT 60)
endforeach()

set(try_assert_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert.xr")
set(try_assert_stdout "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert.stdout")
set(try_assert_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/try_assert.c")
add_custom_command(OUTPUT "${try_assert_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${try_assert_fixture}" "${try_assert_generated}"
    DEPENDS test_xir_from_utf8 "${try_assert_fixture}" VERBATIM)
add_executable(test_xir_try_assert_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${try_assert_generated}")
target_link_libraries(test_xir_try_assert_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_try_assert_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_try_assert_native PRIVATE /utf-8 /W4 /WX)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_try_assert_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_try_assert_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_try_assert_source
    COMMAND test_xir_from_utf8 --vm-file "${try_assert_fixture}" --expect "${try_assert_stdout}")
add_test(NAME test_xir_try_assert_native
    COMMAND test_xir_try_assert_native --expect "${try_assert_stdout}")
set_tests_properties(test_xir_try_assert_source test_xir_try_assert_native PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;panic" TIMEOUT 60)

foreach(mode owner vm resources controls memory rejections)
    add_test(NAME test_xir_try_assert_${mode} COMMAND test_xir_ctfe_slice_u8 try-${mode})
    set_tests_properties(test_xir_try_assert_${mode} PROPERTIES LABELS "unit;xir;ctfe;ownership;panic;resources" TIMEOUT 60)
endforeach()

foreach(case task_unsafe task_safe ordinary_unsafe order)
    set(fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_${case}.xr")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/try_assert_${case}.c")
    set(target "test_xir_try_assert_${case}_native")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${fixture}" "${generated}"
        DEPENDS test_xir_from_utf8 "${fixture}" VERBATIM)
    add_executable(${target} "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${generated}")
    target_link_libraries(${target} PRIVATE xray_xir_scalar xray_compile_resources)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(case STREQUAL "task_unsafe")
        add_test(NAME test_xir_try_assert_${case}_source COMMAND test_xir_from_utf8 --vm-file "${fixture}" --expect-bad-state)
        add_test(NAME ${target} COMMAND ${target} --expect-bad-state)
    else()
        set(oracle "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_${case}.stdout")
        add_test(NAME test_xir_try_assert_${case}_source COMMAND test_xir_from_utf8 --vm-file "${fixture}" --expect "${oracle}")
        add_test(NAME ${target} COMMAND ${target} --expect "${oracle}")
    endif()
    set_tests_properties(test_xir_try_assert_${case}_source ${target} PROPERTIES LABELS "unit;xir;panic;ownership;task" TIMEOUT 120)
endforeach()

add_test(NAME test_xir_try_assert_graph_source COMMAND test_xir_from_utf8 --try-graph)
set_tests_properties(test_xir_try_assert_graph_source PROPERTIES LABELS "unit;xir;panic;ownership" TIMEOUT 120)

add_test(NAME test_xir_try_assert_lexical_source COMMAND test_xir_from_utf8 --try-rejections)
add_test(NAME test_xir_try_assert_never_source COMMAND test_xir_from_utf8 --vm-file "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_never.xr" --expect "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_assert_never.stdout")
set_tests_properties(test_xir_try_assert_lexical_source test_xir_try_assert_never_source PROPERTIES LABELS "unit;xir;panic;errors" TIMEOUT 120)

if(WIN32)
    add_test(NAME test_xir_try_assert_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_try_assert_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --source "${try_assert_fixture}" --expect "${try_assert_stdout}")
    set_tests_properties(test_xir_try_assert_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE PROCESSORS 1
        LABELS "unit;xir;cli;panic;execution;sdk")
endif()

if(WIN32)
    add_test(NAME test_xir_try_optional_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_try_optional_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --source "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_optional.xr"
        --expect "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_optional.expected.json"
        --empty-source "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_optional_never.xr")
    set_tests_properties(test_xir_try_optional_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE PROCESSORS 1
        LABELS "unit;xir;cli;errors;execution;ctfe;sdk")
endif()

add_test(NAME test_xir_try_optional_resources COMMAND test_xir_local_errors optional-resources)
set_tests_properties(test_xir_try_optional_resources PROPERTIES LABELS "unit;xir;errors;ownership;resources" TIMEOUT 60)
add_executable(test_xir_try_optional_faults "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_local_errors.c")
target_link_libraries(test_xir_try_optional_faults PRIVATE xray_xir_source_product xray_xir_vm)
target_compile_definitions(test_xir_try_optional_faults PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib" XR_LOCAL_ERROR_ALLOCATOR=1)
set_target_properties(test_xir_try_optional_faults PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_try_optional_faults PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_try_optional_faults PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_try_optional_faults COMMAND test_xir_try_optional_faults optional-resources)
set_tests_properties(test_xir_try_optional_faults PROPERTIES LABELS "unit;xir;errors;ownership;resources" TIMEOUT 60)

set(try_optional_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_optional.xr")
set(try_optional_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/try_optional.c")
add_custom_command(OUTPUT "${try_optional_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${try_optional_fixture}" "${try_optional_generated}"
    DEPENDS test_xir_from_utf8 "${try_optional_fixture}" VERBATIM)
add_executable(test_xir_try_optional_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${try_optional_generated}")
target_link_libraries(test_xir_try_optional_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_try_optional_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_try_optional_native PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_try_optional_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_try_optional_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_try_optional_generated_native COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_try_optional_cli.py" --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
    --source "${try_optional_fixture}" --expect "${CMAKE_CURRENT_LIST_DIR}/fixtures/try_optional.expected.json"
    --native-driver $<TARGET_FILE:test_xir_try_optional_native>)
set_tests_properties(test_xir_try_optional_generated_native PROPERTIES LABELS "unit;xir;native;ctfe;ownership" TIMEOUT 120)

add_test(NAME test_xir_nullable_unit_source COMMAND test_xir_local_errors nullable-unit)
add_test(NAME test_xir_nullable_unit_resources COMMAND test_xir_local_errors unit-resources)
add_test(NAME test_xir_nullable_unit_faults COMMAND test_xir_try_optional_faults unit-resources)
add_test(NAME test_xir_nullable_unit_values COMMAND test_xir_values --nullable-unit)
set_tests_properties(test_xir_nullable_unit_source test_xir_nullable_unit_resources
    test_xir_nullable_unit_faults test_xir_nullable_unit_values PROPERTIES
    LABELS "unit;xir;nullable;ownership;resources" TIMEOUT 120)
set(nullable_unit_fixture "${CMAKE_CURRENT_LIST_DIR}/fixtures/nullable_unit.xr")
set(nullable_unit_oracle "${CMAKE_CURRENT_LIST_DIR}/fixtures/nullable_unit.expected.json")
if(WIN32)
    add_test(NAME test_xir_nullable_unit_cli COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/test_try_optional_cli.py"
        --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
        --source "${nullable_unit_fixture}" --expect "${nullable_unit_oracle}")
    set_tests_properties(test_xir_nullable_unit_cli PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;xir;nullable;cli;execution;sdk")
endif()
set(nullable_unit_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/nullable_unit.c")
add_custom_command(OUTPUT "${nullable_unit_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file "${nullable_unit_fixture}" "${nullable_unit_generated}"
    DEPENDS test_xir_from_utf8 "${nullable_unit_fixture}" VERBATIM)
add_executable(test_xir_nullable_unit_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_from_utf8_native.c" "${nullable_unit_generated}")
target_link_libraries(test_xir_nullable_unit_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_nullable_unit_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_nullable_unit_native PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_nullable_unit_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_nullable_unit_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_nullable_unit_native COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_try_optional_cli.py" --root "${PROJECT_SOURCE_DIR}" --binary $<TARGET_FILE:xray>
    --source "${nullable_unit_fixture}" --expect "${nullable_unit_oracle}"
    --native-driver $<TARGET_FILE:test_xir_nullable_unit_native>)
set_tests_properties(test_xir_nullable_unit_native PROPERTIES LABELS "unit;xir;nullable;native;ctfe;ownership" TIMEOUT 120)

add_executable(test_xir_cleanup_replaced "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_cleanup_replaced.c")
target_link_libraries(test_xir_cleanup_replaced PRIVATE xray_source_api)
target_compile_definitions(test_xir_cleanup_replaced PRIVATE
    XR_CLEANUP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_CLEANUP_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_cleanup_replaced PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_cleanup_replaced PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cleanup_replaced PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
foreach(cleanup_case source host cancel graph sticky-host hard-reentry task-sendable)
    add_test(NAME test_xir_cleanup_replaced_${cleanup_case} COMMAND test_xir_cleanup_replaced ${cleanup_case})
    set_tests_properties(test_xir_cleanup_replaced_${cleanup_case} PROPERTIES
        LABELS "unit;xir;cleanup;ownership;runtime" TIMEOUT 120)
endforeach()
add_test(NAME test_xir_cleanup_original_optional COMMAND test_xir_local_errors known-cleanup-panic)
set_tests_properties(test_xir_cleanup_original_optional PROPERTIES LABELS "unit;xir;cleanup;errors" TIMEOUT 60)

set(cleanup_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/cleanup_replaced.c")
add_custom_command(OUTPUT "${cleanup_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit-file
        "${CMAKE_CURRENT_LIST_DIR}/fixtures/cleanup_replaced_owned.xr" "${cleanup_generated}"
    DEPENDS test_xir_from_utf8 "${CMAKE_CURRENT_LIST_DIR}/fixtures/cleanup_replaced_owned.xr" VERBATIM)
add_executable(test_xir_cleanup_replaced_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_cleanup_replaced_native.c" "${cleanup_generated}")
target_link_libraries(test_xir_cleanup_replaced_native PRIVATE xray_xir_scalar xray_compile_resources)
set_target_properties(test_xir_cleanup_replaced_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_cleanup_replaced_native PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_cleanup_replaced_native PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_cleanup_replaced_native PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_cleanup_replaced_generated_native COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_cleanup_replaced_cli.py" --root "${PROJECT_SOURCE_DIR}"
    --binary $<TARGET_FILE:xray> --fixtures "${CMAKE_CURRENT_LIST_DIR}/fixtures"
    --native-driver $<TARGET_FILE:test_xir_cleanup_replaced_native>)
add_test(NAME test_xir_cleanup_replaced_cli COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/test_cleanup_replaced_cli.py" --root "${PROJECT_SOURCE_DIR}"
    --binary $<TARGET_FILE:xray> --fixtures "${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_tests_properties(test_xir_cleanup_replaced_generated_native test_xir_cleanup_replaced_cli PROPERTIES
    LABELS "unit;xir;cleanup;ownership;native" RUN_SERIAL TRUE TIMEOUT 240)

add_executable(test_xir_cleanup_replaced_wait_fault "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_cleanup_replaced.c")
target_link_libraries(test_xir_cleanup_replaced_wait_fault PRIVATE xray_source_api)
target_compile_definitions(test_xir_cleanup_replaced_wait_fault PRIVATE XR_CLEANUP_CALL_OBSERVER=1
    XR_CLEANUP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib" XR_CLEANUP_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_cleanup_replaced_wait_fault PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_cleanup_replaced_wait_fault PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cleanup_replaced_wait_fault PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_cleanup_replaced_wait_identity COMMAND test_xir_cleanup_replaced_wait_fault task-identity)
add_test(NAME test_xir_cleanup_replaced_task_shutdown COMMAND test_xir_cleanup_replaced_wait_fault task-shutdown)
add_test(NAME test_xir_cleanup_replaced_task_pair COMMAND test_xir_cleanup_replaced_wait_fault task-pair)
add_test(NAME test_xir_cleanup_failed_module COMMAND test_xir_cleanup_replaced_wait_fault failed-module)
set_tests_properties(test_xir_cleanup_failed_module PROPERTIES LABELS "unit;xir;cleanup;ownership;runtime" TIMEOUT 120)
add_test(NAME test_xir_cleanup_replaced_task_positive COMMAND test_xir_cleanup_replaced task-identity)
add_test(NAME test_xir_cleanup_replaced_late_cancel COMMAND test_xir_cleanup_replaced late-cancel)
set_tests_properties(test_xir_cleanup_replaced_wait_identity test_xir_cleanup_replaced_task_shutdown test_xir_cleanup_replaced_task_pair test_xir_cleanup_replaced_task_positive test_xir_cleanup_replaced_late_cancel
    PROPERTIES LABELS "unit;xir;cleanup;ownership;runtime" TIMEOUT 120)

add_executable(test_xir_cleanup_replaced_host_fault "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_cleanup_replaced.c")
target_link_libraries(test_xir_cleanup_replaced_host_fault PRIVATE xray_source_api)
target_compile_definitions(test_xir_cleanup_replaced_host_fault PRIVATE XR_CLEANUP_HOST_OBSERVER=1
    XR_CLEANUP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib" XR_CLEANUP_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_cleanup_replaced_host_fault PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_cleanup_replaced_host_fault PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cleanup_replaced_host_fault PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_cleanup_replaced_host_wait COMMAND test_xir_cleanup_replaced_host_fault host-wait)
set_tests_properties(test_xir_cleanup_replaced_host_wait PROPERTIES LABELS "unit;xir;cleanup;ownership;runtime" TIMEOUT 120)

add_executable(test_xir_cleanup_replaced_value_fault "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_cleanup_replaced.c")
target_link_libraries(test_xir_cleanup_replaced_value_fault PRIVATE xray_source_api)
target_compile_definitions(test_xir_cleanup_replaced_value_fault PRIVATE XR_CLEANUP_VALUE_OBSERVER=1
    XR_CLEANUP_STDLIB="${PROJECT_SOURCE_DIR}/stdlib" XR_CLEANUP_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(test_xir_cleanup_replaced_value_fault PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_cleanup_replaced_value_fault PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_cleanup_replaced_value_fault PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_cleanup_replaced_value_fault PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_xir_cleanup_replaced_resources COMMAND test_xir_cleanup_replaced_value_fault resources)
set_tests_properties(test_xir_cleanup_replaced_resources PROPERTIES LABELS "unit;xir;cleanup;ownership;resources" TIMEOUT 120)
