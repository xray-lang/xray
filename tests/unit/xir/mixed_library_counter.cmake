add_executable(test_xir_mixed_library_producer xir/test_xir_mixed_library_producer.c)
include(${CMAKE_CURRENT_LIST_DIR}/mixed_task_results.cmake)
add_executable(test_xir_effects_callable_bounds xir/test_xir_effects_callable_bounds.c)
target_link_libraries(test_xir_effects_callable_bounds PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_effects_callable_bounds PRIVATE
    XR_EFFECT_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_effect_source")
set_target_properties(test_xir_effects_callable_bounds PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_effects_callable_bounds PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_effects_callable_bounds PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_effects_callable_bounds COMMAND test_xir_effects_callable_bounds)
set_tests_properties(test_xir_effects_callable_bounds PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
    LABELS "unit;xir;mixed;source;ownership;effects;callable;generated-c")
add_executable(test_xir_source_array_packets xir/test_xir_source_array_packets.c)
target_link_libraries(test_xir_source_array_packets PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_source_array_packets PRIVATE
    XR_ARRAY_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_source"
    XR_ARRAY_REORDER_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_array_reordering_source")
foreach(array_producer IN ITEMS test_xir_source_array_packets)
    set_target_properties(${array_producer} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${array_producer} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${array_producer} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

set(array_query_fi_begin 0)
while(array_query_fi_begin LESS 9500)
    math(EXPR array_query_fi_end "${array_query_fi_begin} + 64")
    if(array_query_fi_end GREATER 9500)
        set(array_query_fi_end 9500)
    endif()

    set(array_query_fi_begin ${array_query_fi_end})
endwhile()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-counter-absent")
target_link_libraries(test_xir_mixed_library_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_library_producer PRIVATE
    XR_COUNTER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_library_counter"
    XR_COUNTER_LOGICAL_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-counter-absent")
set(XIR_LIBRARY_COUNTER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_library_counter.c")
add_custom_command(OUTPUT "${XIR_LIBRARY_COUNTER_GENERATED}"
    COMMAND $<TARGET_FILE:test_xir_mixed_library_producer> "${XIR_LIBRARY_COUNTER_GENERATED}"
    DEPENDS test_xir_mixed_library_producer xir/fixtures/p20_library_counter/root.xr xir/fixtures/p20_library_counter/library.xr
    VERBATIM)

foreach(counter_target IN ITEMS test_xir_mixed_library_producer)
    set_target_properties(${counter_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${counter_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${counter_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_executable(test_xir_mixed_library_pure_producer xir/test_xir_mixed_library_producer.c)
target_link_libraries(test_xir_mixed_library_pure_producer PRIVATE xray_xir_source xray_xir_cgen)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-pure-absent")
target_compile_definitions(test_xir_mixed_library_pure_producer PRIVATE
    XR_COUNTER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_library_pure_worker"
    XR_COUNTER_LOGICAL_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-pure-absent")
set(library_pure_source "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library_pure_worker.c")
add_custom_command(OUTPUT "${library_pure_source}"
    COMMAND $<TARGET_FILE:test_xir_mixed_library_pure_producer> "${library_pure_source}"
    DEPENDS test_xir_mixed_library_pure_producer xir/fixtures/p20_library_pure_worker/root.xr xir/fixtures/p20_library_pure_worker/library.xr VERBATIM)

foreach(library_pure_target IN ITEMS test_xir_mixed_library_pure_producer)
    set_target_properties(${library_pure_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${library_pure_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${library_pure_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-permissions-absent")

set(permission_fi_begin 0)
while(permission_fi_begin LESS 4279)
    math(EXPR permission_fi_end "${permission_fi_begin} + 64")
    if(permission_fi_end GREATER 4279)
        set(permission_fi_end 4279)
    endif()

    set(permission_fi_begin ${permission_fi_end})
endwhile()
