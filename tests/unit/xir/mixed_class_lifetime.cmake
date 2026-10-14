add_executable(test_xir_mixed_class_producer xir/test_xir_mixed_class_producer.c)
target_link_libraries(test_xir_mixed_class_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_class_producer PRIVATE
    XR_CLASS_LIFETIME_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_retained_class")
set(XIR_CLASS_LIFETIME_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_class_lifetime.c")
add_custom_command(OUTPUT "${XIR_CLASS_LIFETIME_GENERATED}"
    COMMAND $<TARGET_FILE:test_xir_mixed_class_producer> "${XIR_CLASS_LIFETIME_GENERATED}"
    DEPENDS test_xir_mixed_class_producer xir/fixtures/p20_retained_class/root.xr xir/fixtures/p20_retained_class/callee.xr
    VERBATIM)
add_executable(test_xir_mixed_class_lifetime xir/test_xir_mixed_class_lifetime.c "${XIR_CLASS_LIFETIME_GENERATED}")
target_link_libraries(test_xir_mixed_class_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_class_resources xir/test_xir_mixed_class_resources.c "${XIR_CLASS_LIFETIME_GENERATED}")
target_link_libraries(test_xir_mixed_class_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(class_lifetime_target IN ITEMS test_xir_mixed_class_producer test_xir_mixed_class_lifetime test_xir_mixed_class_resources)
    set_target_properties(${class_lifetime_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${class_lifetime_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${class_lifetime_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(class_lifetime_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_class_lifetime_${class_lifetime_backend} COMMAND test_xir_mixed_class_lifetime ${class_lifetime_backend})
    set_tests_properties(test_xir_mixed_class_lifetime_${class_lifetime_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;class")
    foreach(class_resource_mode IN ITEMS axes fi)
        add_test(NAME test_xir_mixed_class_${class_lifetime_backend}_${class_resource_mode}
            COMMAND test_xir_mixed_class_resources ${class_lifetime_backend} ${class_resource_mode})
        set_tests_properties(test_xir_mixed_class_${class_lifetime_backend}_${class_resource_mode} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
            LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;class;${class_resource_mode}")
    endforeach()
endforeach()
