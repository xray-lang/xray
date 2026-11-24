add_executable(test_native_managed_public managed_public_client.c)
target_link_libraries(test_native_managed_public PRIVATE xray_native_runtime)
set_target_properties(test_native_managed_public PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_managed_public PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_managed_public PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_managed_public PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_managed_public COMMAND test_native_managed_public)
set_tests_properties(native_managed_public PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)
set(managed_public_provider gnu)
if(MSVC)
    set(managed_public_provider msvc)
endif()
add_test(NAME native_managed_public_native COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_managed_public_native.py"
    --compiler "${CMAKE_C_COMPILER}" --kind "${managed_public_provider}"
    --source "${PROJECT_SOURCE_DIR}" --work "${CMAKE_CURRENT_BINARY_DIR}/managed-public")
set_tests_properties(native_managed_public_native PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_capture_backing test_capture_backing.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_map_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_capture_backing.c")
target_include_directories(test_native_capture_backing PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_capture_backing PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_capture_backing PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_capture_backing PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_capture_backing PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_capture_backing PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_capture_backing COMMAND test_native_capture_backing)
set_tests_properties(native_capture_backing PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_value_plan test_value_plan.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_image.c")
target_include_directories(test_native_value_plan PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_value_plan PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_value_plan PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_value_plan PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_value_plan PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_value_plan PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_value_plan COMMAND test_native_value_plan)
set_tests_properties(native_value_plan PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_class_identity test_class_identity.c)
target_link_libraries(test_native_class_identity PRIVATE xray_native_runtime)
target_include_directories(test_native_class_identity PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_class_identity PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_class_identity PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_class_identity PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_class_identity PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_class_identity COMMAND test_native_class_identity)
set_tests_properties(native_class_identity PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_managed_status test_managed_status.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_ops.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_status.c")
target_include_directories(test_native_managed_status PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_managed_status PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_managed_status PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_managed_status PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_managed_status PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_managed_status PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_managed_status COMMAND test_native_managed_status)
set_tests_properties(native_managed_status PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_place test_place.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c")
target_include_directories(test_native_place PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_place PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_place PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_place PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_place PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_place PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_place COMMAND test_native_place)
set_tests_properties(native_place PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_array_slice test_array_slice.c)
target_link_libraries(test_native_array_slice PRIVATE xray_native_runtime)
target_include_directories(test_native_array_slice PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_array_slice PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_array_slice PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_array_slice PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_array_slice PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_array_slice COMMAND test_native_array_slice)
set_tests_properties(native_array_slice PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_string_backing_ops test_string_backing_ops.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing_ops.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c")
target_include_directories(test_native_string_backing_ops PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_string_backing_ops PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_string_backing_ops PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_string_backing_ops PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_string_backing_ops PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_string_backing_ops PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_string_backing_ops COMMAND test_native_string_backing_ops)
set_tests_properties(native_string_backing_ops PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_backing test_backing.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_ops.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_map_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_set_backing.c")
target_include_directories(test_native_backing PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_backing PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_backing PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_backing PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_backing PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_backing PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_backing COMMAND test_native_backing)
set_tests_properties(native_backing PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_managed_kernel test_managed_kernel.c)
target_link_libraries(test_native_managed_kernel PRIVATE xray_native_runtime)
if(WIN32)
    target_sources(test_native_managed_kernel PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    find_package(Threads REQUIRED)
    target_sources(test_native_managed_kernel PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_native_managed_kernel PRIVATE Threads::Threads)
    target_compile_definitions(test_native_managed_kernel PRIVATE _POSIX_C_SOURCE=200809L)
endif()
target_include_directories(test_native_managed_kernel PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_managed_kernel PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_managed_kernel PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_managed_kernel PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_managed_kernel PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_managed_kernel COMMAND test_native_managed_kernel)
set_tests_properties(native_managed_kernel PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

add_executable(test_native_managed_root test_managed_root.c)
target_link_libraries(test_native_managed_root PRIVATE xray_native_runtime)
target_include_directories(test_native_managed_root PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_managed_root PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_managed_root PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_managed_root PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_managed_root PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_managed_root COMMAND test_native_managed_root)
set_tests_properties(native_managed_root PROPERTIES LABELS "unit;native-runtime;ownership" TIMEOUT 30)

set(native_runtime_clients)
add_executable(test_native_f64_i64_conversion test_f64_i64_conversion.c)
target_link_libraries(test_native_f64_i64_conversion PRIVATE xray_lir_bytecode xray_lir_c xray_native_runtime)
target_include_directories(test_native_f64_i64_conversion PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_f64_i64_conversion PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_f64_i64_conversion PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_native_f64_i64_conversion PRIVATE -Wall -Wextra -Werror -pedantic)
    target_link_libraries(test_native_f64_i64_conversion PRIVATE m)
endif()
add_test(NAME native_f64_i64_conversion COMMAND test_native_f64_i64_conversion)
set_tests_properties(native_f64_i64_conversion PROPERTIES LABELS "unit;native-runtime;float" TIMEOUT 30)
add_executable(test_native_f64_text test_f64_text.c)
target_link_libraries(test_native_f64_text PRIVATE xray_lir_bytecode xray_native_runtime)
target_include_directories(test_native_f64_text PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_f64_text PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_f64_text PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_native_f64_text PRIVATE -Wall -Wextra -Werror -pedantic)
    target_link_libraries(test_native_f64_text PRIVATE m)
endif()
add_test(NAME native_f64_text COMMAND test_native_f64_text)
set_tests_properties(native_f64_text PROPERTIES LABELS "unit;native-runtime;ownership;float" TIMEOUT 30)
add_executable(test_native_module_state test_module_state.c
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_module_state.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_capture_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_managed_root.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_place.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_plan.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_value_image.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_backing_types.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_string_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_map_backing.c"
    "${PROJECT_SOURCE_DIR}/src/runtime/native/xnative_array_ops.c")
target_include_directories(test_native_module_state PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_module_state PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_module_state PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_module_state PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_definitions(test_native_module_state PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(test_native_module_state PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_module_state_owner COMMAND test_native_module_state)
set_tests_properties(native_module_state_owner PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 30)

add_executable(test_native_locations test_locations.c)
target_link_libraries(test_native_locations PRIVATE xray_native_runtime)
target_include_directories(test_native_locations PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_locations PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_locations PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_native_locations PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_location_owner COMMAND test_native_locations)
set_tests_properties(native_location_owner PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 10)

add_executable(test_native_execution test_execution.c)
# Standalone kernels still compile their own owner implementation. Complete
# their host storage references from the production archive.
foreach(target test_native_capture_backing test_native_value_plan test_native_managed_status
        test_native_string_backing_ops test_native_backing test_native_module_state)
    target_link_libraries(${target} PRIVATE xray_native_runtime)
endforeach()
target_link_libraries(test_native_execution PRIVATE xray_native_runtime)
target_include_directories(test_native_execution PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_execution PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_execution PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_execution PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_execution PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_execution_scope COMMAND test_native_execution)
set_tests_properties(native_execution_scope PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 10)

add_executable(test_native_execution_allocations test_execution.c)
target_compile_definitions(test_native_execution_allocations PRIVATE XR_NATIVE_VALUE_ALLOCATIONS=1)
target_link_libraries(test_native_execution_allocations PRIVATE xray_native_runtime)
target_include_directories(test_native_execution_allocations PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_execution_allocations PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_execution_allocations PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_execution_allocations PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_native_execution_allocations PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_execution_string_allocations COMMAND test_native_execution_allocations)
set_tests_properties(native_execution_string_allocations PROPERTIES LABELS "unit;native-runtime;ownership;fault-injection" TIMEOUT 30)

foreach(kind RANGE 0 9)
    set(client "test_native_runtime_${kind}")
    add_executable(${client} client.c)
    target_compile_definitions(${client} PRIVATE MAP_KIND=${kind})
    target_link_libraries(${client} PRIVATE xray_native_runtime)
    target_include_directories(${client} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(${client} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${client} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${client} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    list(APPEND native_runtime_clients ${client})
endforeach()
add_custom_target(test_native_runtime_clients DEPENDS ${native_runtime_clients})

# These targets must fail at the real link boundary; they are never in ALL.
add_executable(test_native_runtime_missing EXCLUDE_FROM_ALL link_client.c)
target_link_libraries(test_native_runtime_missing PRIVATE xray_native_runtime)
add_executable(test_native_runtime_duplicate EXCLUDE_FROM_ALL link_client.c map.c map_duplicate.c)
target_link_libraries(test_native_runtime_duplicate PRIVATE xray_native_runtime)

foreach(suite output failures link_contract)
    add_test(NAME native_runtime_${suite}
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.py"
            --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS} --suite ${suite} --build "${CMAKE_BINARY_DIR}"
            --clients "$<TARGET_FILE:test_native_runtime_0>" "$<TARGET_FILE:test_native_runtime_1>"
                "$<TARGET_FILE:test_native_runtime_2>" "$<TARGET_FILE:test_native_runtime_3>"
                "$<TARGET_FILE:test_native_runtime_4>" "$<TARGET_FILE:test_native_runtime_5>"
                "$<TARGET_FILE:test_native_runtime_6>" "$<TARGET_FILE:test_native_runtime_7>"
                "$<TARGET_FILE:test_native_runtime_8>" "$<TARGET_FILE:test_native_runtime_9>"
            --logs "${CMAKE_CURRENT_BINARY_DIR}/results-${suite}")
    set_tests_properties(native_runtime_${suite} PROPERTIES
        LABELS "unit;native-runtime;fast" TIMEOUT 90 RUN_SERIAL TRUE)
endforeach()

add_executable(test_native_clock test_clock.c)
target_link_libraries(test_native_clock PRIVATE xray_lir_bytecode xray_lir_lower xray_lir_c)
target_include_directories(test_native_clock PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_native_clock PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_native_clock PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_native_clock PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME native_clock_scope COMMAND test_native_clock)
set_tests_properties(native_clock_scope PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 30)
add_test(NAME native_clock_standalone
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.py"
        --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS} --suite clock --build "${CMAKE_BINARY_DIR}" --clients "$<TARGET_FILE:test_native_clock>"
        --logs "${CMAKE_CURRENT_BINARY_DIR}/results-clock")
set_tests_properties(native_clock_standalone PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 30)

add_test(NAME native_offset_scope COMMAND test_native_clock offset)
set_tests_properties(native_offset_scope PROPERTIES LABELS "unit;native-runtime;fast" TIMEOUT 30)
