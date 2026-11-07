add_executable(test_xir_generator_driver xir/test_xir_generator_driver.c)
target_link_libraries(test_xir_generator_driver PRIVATE xray_xir_vm)
if(MSVC)
    target_compile_options(test_xir_generator_driver PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_driver PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_driver COMMAND test_xir_generator_driver)
add_test(NAME test_xir_generator_driver_allocations COMMAND test_xir_generator_driver --faults)
set_tests_properties(test_xir_generator_driver_allocations PROPERTIES LABELS "unit;xir;generator;ownership;runtime;fault-injection" TIMEOUT 120 PROCESSORS 1)
set_tests_properties(test_xir_generator_driver PROPERTIES LABELS "unit;xir;generator;ownership;runtime" TIMEOUT 120 PROCESSORS 1)

add_executable(test_xir_generator_source xir/test_xir_generator_source.c)
target_link_libraries(test_xir_generator_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_source COMMAND test_xir_generator_source)
set_tests_properties(test_xir_generator_source PROPERTIES LABELS "unit;xir;source;generator;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_SOURCE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_source.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_SOURCE_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_source> "${XIR_GENERATOR_SOURCE_C}"
    DEPENDS test_xir_generator_source VERBATIM)

add_executable(test_xir_generator_cleanup_source xir/test_xir_generator_cleanup_source.c)
target_link_libraries(test_xir_generator_cleanup_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_cleanup_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_GENERATOR_CLEANUP_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_cleanup.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_CLEANUP_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_cleanup_source> "${XIR_GENERATOR_CLEANUP_C}"
    DEPENDS test_xir_generator_cleanup_source VERBATIM)

foreach(generator_cleanup_target IN ITEMS test_xir_generator_cleanup_source)
    if(MSVC)
        target_compile_options(${generator_cleanup_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${generator_cleanup_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${generator_cleanup_target} COMMAND ${generator_cleanup_target})
    set_tests_properties(${generator_cleanup_target} PROPERTIES LABELS "unit;xir;generator;cleanup;ownership;runtime" TIMEOUT 120 PROCESSORS 1)
endforeach()

add_executable(test_xir_generator_factory_source xir/test_xir_generator_factory_source.c)
target_link_libraries(test_xir_generator_factory_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_factory_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_GENERATOR_FACTORY_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_factory.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_FACTORY_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_factory_source> "${XIR_GENERATOR_FACTORY_C}"
    DEPENDS test_xir_generator_factory_source VERBATIM)

foreach(generator_factory_target IN ITEMS test_xir_generator_factory_source)
    if(MSVC)
        target_compile_options(${generator_factory_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${generator_factory_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${generator_factory_target} COMMAND ${generator_factory_target})
    set_tests_properties(${generator_factory_target} PROPERTIES LABELS "unit;xir;generator;ownership;runtime" TIMEOUT 120 PROCESSORS 1)
endforeach()

set(XIR_GENERATOR_CONSUMER_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_consumer.c")

add_executable(test_xir_iterator_pull_packets xir/test_xir_iterator_pull_packets.c)
target_link_libraries(test_xir_iterator_pull_packets PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_iterator_pull_packets PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_iterator_pull_packets PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_iterator_pull_packets COMMAND test_xir_iterator_pull_packets)
set_tests_properties(test_xir_iterator_pull_packets PROPERTIES LABELS "unit;xir;generator;abi;compiler" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_WAIT_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_wait.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_resource_timeout 120)
        else()
            set(generator_resource_timeout 300)
        endif()

    endforeach()
endforeach()

set(XIR_GENERATOR_CLOSURE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_closure.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_cell_resource_timeout 120)
        else()
            set(generator_cell_resource_timeout 300)
        endif()

    endforeach()
endforeach()

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_nth_resource_timeout 120)
        else()
            set(generator_nth_resource_timeout 300)
        endif()

    endforeach()
endforeach()

set(XIR_GENERATOR_LOOP_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_loop.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_loop_resource_timeout 120)
        else()
            set(generator_loop_resource_timeout 300)
        endif()

    endforeach()
endforeach()

add_executable(test_xir_generator_worker_source xir/test_xir_generator_worker_source.c)
target_link_libraries(test_xir_generator_worker_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_worker_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_worker_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_worker_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_worker_source COMMAND test_xir_generator_worker_source)
set_tests_properties(test_xir_generator_worker_source PROPERTIES LABELS "unit;xir;generator;ownership;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_WORKER_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_worker.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_WORKER_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_worker_source> "${XIR_GENERATOR_WORKER_C}"
    DEPENDS test_xir_generator_worker_source VERBATIM)

set(XIR_GENERATOR_ITERABLE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_iterable.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_iterable_resource_timeout 120)
        else()
            set(generator_iterable_resource_timeout 300)
        endif()

    endforeach()
endforeach()

add_executable(test_xir_generator_method_factory_source xir/test_xir_generator_method_factory_source.c)
target_link_libraries(test_xir_generator_method_factory_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_method_factory_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_method_factory_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_method_factory_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_method_factory_source COMMAND test_xir_generator_method_factory_source)
set_tests_properties(test_xir_generator_method_factory_source PROPERTIES LABELS "unit;xir;generator;ownership;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_METHOD_FACTORY_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_method_factory.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_METHOD_FACTORY_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_method_factory_source> "${XIR_GENERATOR_METHOD_FACTORY_C}"
    DEPENDS test_xir_generator_method_factory_source VERBATIM)

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_method_factory_resource_timeout 120)
        else()
            set(generator_method_factory_resource_timeout 300)
        endif()

    endforeach()
endforeach()

set(XIR_GENERATOR_CALLABLE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_callable.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_callable_resource_timeout 120)
        else()
            set(generator_callable_resource_timeout 300)
        endif()

    endforeach()
endforeach()

set(XIR_GENERATOR_RECEIVER_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_receiver.c")

foreach(provider native vm vm-native native-vm)
    foreach(mode measure axes faults cancel)

        if(mode STREQUAL "measure" OR mode STREQUAL "axes")
            set(generator_receiver_resource_timeout 120)
        else()
            set(generator_receiver_resource_timeout 300)
        endif()

    endforeach()
endforeach()

add_test(NAME test_xir_generator_receiver_compiler_history_reports
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_receiver_compiler_history_reports.py")
set_tests_properties(test_xir_generator_receiver_compiler_history_reports PROPERTIES
    LABELS "unit;xir;generator;compiler-fault;ownership" TIMEOUT 30 PROCESSORS 1)

add_executable(test_xir_generator_producer_sites xir/test_xir_generator_producer_sites.c)
target_link_libraries(test_xir_generator_producer_sites PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_generator_producer_sites PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_producer_sites PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_producer_sites COMMAND test_xir_generator_producer_sites)
set_tests_properties(test_xir_generator_producer_sites PROPERTIES
    LABELS "unit;xir;generator;metadata;ownership;compiler-fault" TIMEOUT 120 PROCESSORS 1)

add_executable(test_xir_generator_consumption_source xir/test_xir_generator_consumption_source.c)
target_link_libraries(test_xir_generator_consumption_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_consumption_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_consumption_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_consumption_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_consumption_source COMMAND test_xir_generator_consumption_source)
set_tests_properties(test_xir_generator_consumption_source PROPERTIES
    LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_CONSUMPTION_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_consumption.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_CONSUMPTION_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_consumption_source> "${XIR_GENERATOR_CONSUMPTION_C}"
    DEPENDS test_xir_generator_consumption_source VERBATIM)

add_executable(test_xir_generator_capture_source xir/test_xir_generator_capture_source.c)
target_link_libraries(test_xir_generator_capture_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_capture_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_capture_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_capture_source PRIVATE -Wall -Wextra -Werror)
endif()
foreach(capture iterator_argument iterator_capture callable_capture physical_walk physical_unknown_bound)
    add_test(NAME test_xir_generator_capture_source_${capture} COMMAND test_xir_generator_capture_source ${capture})
    set_tests_properties(test_xir_generator_capture_source_${capture} PROPERTIES
        LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)
endforeach()

set(XIR_GENERATOR_CAPTURE_C "${CMAKE_CURRENT_BINARY_DIR}/generator-capture/generator_capture_native.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_CAPTURE_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generator-capture"
    COMMAND $<TARGET_FILE:test_xir_generator_capture_source> physical_walk "${XIR_GENERATOR_CAPTURE_C}"
    DEPENDS test_xir_generator_capture_source
    VERBATIM)

add_executable(test_xir_generator_role_source xir/test_xir_generator_role_source.c)
target_link_libraries(test_xir_generator_role_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_role_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_GENERATOR_ROLE_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_role.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_ROLE_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_role_source> "${XIR_GENERATOR_ROLE_C}"
    DEPENDS test_xir_generator_role_source VERBATIM)
add_executable(test_xir_generator_role_runtime xir/test_xir_generator_role_runtime.c "${XIR_GENERATOR_ROLE_C}")
target_link_libraries(test_xir_generator_role_runtime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(target test_xir_generator_role_source test_xir_generator_role_runtime)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_generator_role_source COMMAND test_xir_generator_role_source)
set_tests_properties(test_xir_generator_role_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)
foreach(mode native vm vm-native native-vm)
    add_test(NAME test_xir_generator_role_${mode} COMMAND test_xir_generator_role_runtime ${mode})
    set_tests_properties(test_xir_generator_role_${mode} PROPERTIES LABELS "unit;xir;generator;ownership;runtime;mixed" TIMEOUT 120 PROCESSORS 1)
    add_test(NAME test_xir_generator_role_ordinary_${mode} COMMAND test_xir_generator_role_runtime ${mode} --ordinary)
    set_tests_properties(test_xir_generator_role_ordinary_${mode} PROPERTIES LABELS "unit;xir;generator;ownership;runtime;mixed" TIMEOUT 120 PROCESSORS 1)
endforeach()

add_executable(test_xir_generator_terminal_source xir/test_xir_generator_terminal_source.c)
target_link_libraries(test_xir_generator_terminal_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_terminal_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_terminal_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_terminal_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_terminal_source COMMAND test_xir_generator_terminal_source)
set_tests_properties(test_xir_generator_terminal_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_TERMINAL_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_terminal.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_TERMINAL_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_terminal_source> "${XIR_GENERATOR_TERMINAL_C}"
    DEPENDS test_xir_generator_terminal_source VERBATIM)

add_test(NAME test_xir_generator_cold_argument_source COMMAND test_xir_generator_terminal_source --inferred)
set_tests_properties(test_xir_generator_cold_argument_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_TERMINAL_INFERRED_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_terminal_inferred.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_TERMINAL_INFERRED_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_terminal_source> --inferred "${XIR_GENERATOR_TERMINAL_INFERRED_C}"
    DEPENDS test_xir_generator_terminal_source VERBATIM)

add_test(NAME test_xir_generator_terminal_workers_source COMMAND test_xir_generator_terminal_source --inferred-workers)
set_tests_properties(test_xir_generator_terminal_workers_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

add_test(NAME test_xir_generator_terminal_function_worker_source COMMAND test_xir_generator_terminal_source --inferred-function-worker)
set_tests_properties(test_xir_generator_terminal_function_worker_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_TERMINAL_FNWORKER_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_terminal_fnworker.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_TERMINAL_FNWORKER_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_terminal_source> --inferred-function-worker "${XIR_GENERATOR_TERMINAL_FNWORKER_C}"
    DEPENDS test_xir_generator_terminal_source VERBATIM)

add_executable(test_xir_generator_cell_borrow_source xir/test_xir_generator_cell_borrow_source.c)
target_link_libraries(test_xir_generator_cell_borrow_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_generator_cell_borrow_source PRIVATE XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_generator_cell_borrow_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_generator_cell_borrow_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generator_cell_borrow_source COMMAND test_xir_generator_cell_borrow_source)
set_tests_properties(test_xir_generator_cell_borrow_source PROPERTIES LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)

set(XIR_GENERATOR_CELL_BORROW_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_cell_borrow.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_CELL_BORROW_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_cell_borrow_source> "${XIR_GENERATOR_CELL_BORROW_C}"
    DEPENDS test_xir_generator_cell_borrow_source VERBATIM)

add_test(NAME test_xir_generator_cell_weakened_source COMMAND test_xir_generator_cell_borrow_source --explicit-root)
set_tests_properties(test_xir_generator_cell_weakened_source PROPERTIES
    LABELS "unit;xir;generator;ownership;compiler;runtime" TIMEOUT 120 PROCESSORS 1)
set(XIR_GENERATOR_CELL_WEAKENED_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_generator_cell_weakened.c")
add_custom_command(OUTPUT "${XIR_GENERATOR_CELL_WEAKENED_C}"
    COMMAND $<TARGET_FILE:test_xir_generator_cell_borrow_source> --explicit-root "${XIR_GENERATOR_CELL_WEAKENED_C}"
    DEPENDS test_xir_generator_cell_borrow_source VERBATIM)

include("${CMAKE_CURRENT_LIST_DIR}/mixed_iterator_lifetime.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/mixed_class_lifetime.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/mixed_callback_boundary.cmake")
