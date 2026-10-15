add_executable(test_xir_mixed_string_worker_producer xir/test_xir_mixed_string_worker_producer.c)
target_link_libraries(test_xir_mixed_string_worker_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_string_worker_producer PRIVATE
    XR_STRING_WORKER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_worker_owned_string")
set(XIR_STRING_WORKER_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_string_worker.c")
add_custom_command(OUTPUT "${XIR_STRING_WORKER_GENERATED}"
    COMMAND $<TARGET_FILE:test_xir_mixed_string_worker_producer> "${XIR_STRING_WORKER_GENERATED}"
    DEPENDS test_xir_mixed_string_worker_producer xir/fixtures/p20_worker_owned_string/root.xr xir/fixtures/p20_worker_owned_string/callee.xr
    VERBATIM)
add_executable(test_xir_mixed_string_worker xir/test_xir_mixed_string_worker.c "${XIR_STRING_WORKER_GENERATED}")
target_link_libraries(test_xir_mixed_string_worker PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_string_resources xir/test_xir_mixed_string_resources.c "${XIR_STRING_WORKER_GENERATED}")
target_link_libraries(test_xir_mixed_string_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(string_worker_target IN ITEMS test_xir_mixed_string_worker_producer test_xir_mixed_string_worker test_xir_mixed_string_resources)
    set_target_properties(${string_worker_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${string_worker_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${string_worker_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(string_worker_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_string_worker_${string_worker_backend} COMMAND test_xir_mixed_string_worker ${string_worker_backend})
    set_tests_properties(test_xir_mixed_string_worker_${string_worker_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;root;worker;ownership;generated-c;execution;lifecycle;task;string")
    foreach(string_worker_gate IN ITEMS axes fi)
        add_test(NAME test_xir_mixed_string_worker_${string_worker_backend}_${string_worker_gate}
            COMMAND test_xir_mixed_string_resources ${string_worker_backend} ${string_worker_gate})
        set_tests_properties(test_xir_mixed_string_worker_${string_worker_backend}_${string_worker_gate} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;root;worker;ownership;generated-c;execution;lifecycle;task;string;resources;${string_worker_gate}")
    endforeach()
endforeach()
