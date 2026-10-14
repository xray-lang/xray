add_executable(test_xir_mixed_worker_producer xir/test_xir_mixed_worker_producer.c)
target_link_libraries(test_xir_mixed_worker_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_worker_producer PRIVATE
    XR_WORKER_CONTROL_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_worker_controls")
set(worker_control_names pure-control local-worker-control snapshot-control generic-pure-control alias-pure-control cross-control mutual-control)
set(worker_control_generated)
foreach(worker_control_index RANGE 0 6)
    list(GET worker_control_names ${worker_control_index} worker_control_name)
    set(worker_control_source "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_worker_control_${worker_control_index}.c")
    add_custom_command(OUTPUT "${worker_control_source}"
        COMMAND $<TARGET_FILE:test_xir_mixed_worker_producer> ${worker_control_index} "${worker_control_source}"
        DEPENDS test_xir_mixed_worker_producer xir/fixtures/p20_worker_controls/${worker_control_name}.xr
            xir/fixtures/p20_worker_controls/shared.xr VERBATIM)
    list(APPEND worker_control_generated "${worker_control_source}")
endforeach()
add_executable(test_xir_mixed_worker_controls xir/test_xir_mixed_worker_controls.c ${worker_control_generated})
target_link_libraries(test_xir_mixed_worker_controls PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_worker_resources xir/test_xir_mixed_worker_resources.c ${worker_control_generated})
target_link_libraries(test_xir_mixed_worker_resources PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_worker_cancel xir/test_xir_mixed_worker_cancel.c ${worker_control_generated})
target_link_libraries(test_xir_mixed_worker_cancel PRIVATE xray_xir_vm xray_xir_scalar)
foreach(worker_control_target IN ITEMS test_xir_mixed_worker_producer test_xir_mixed_worker_controls test_xir_mixed_worker_resources test_xir_mixed_worker_cancel)
    set_target_properties(${worker_control_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${worker_control_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${worker_control_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(worker_control_name IN LISTS worker_control_names)
    foreach(worker_control_backend IN ITEMS 0 1 vm native)
        add_test(NAME test_xir_mixed_worker_${worker_control_name}_${worker_control_backend}
            COMMAND test_xir_mixed_worker_controls ${worker_control_name} ${worker_control_backend})
        set_tests_properties(test_xir_mixed_worker_${worker_control_name}_${worker_control_backend} PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;root;worker;ownership;generated-c;execution")
        add_test(NAME test_xir_mixed_worker_${worker_control_name}_${worker_control_backend}_cancel
            COMMAND test_xir_mixed_worker_cancel ${worker_control_name} ${worker_control_backend})
        set_tests_properties(test_xir_mixed_worker_${worker_control_name}_${worker_control_backend}_cancel PROPERTIES
            TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;root;worker;ownership;generated-c;execution;cancel")
        foreach(worker_resource_mode IN ITEMS axes fi)
            add_test(NAME test_xir_mixed_worker_${worker_control_name}_${worker_control_backend}_${worker_resource_mode}
                COMMAND test_xir_mixed_worker_resources ${worker_control_name} ${worker_control_backend} ${worker_resource_mode})
            set_tests_properties(test_xir_mixed_worker_${worker_control_name}_${worker_control_backend}_${worker_resource_mode} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE COST 1 LABELS "unit;xir;mixed;root;worker;ownership;generated-c;execution;${worker_resource_mode}")
        endforeach()
    endforeach()
endforeach()
