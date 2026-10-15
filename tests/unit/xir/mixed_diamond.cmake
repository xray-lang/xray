add_executable(test_xir_mixed_diamond_producer xir/test_xir_mixed_diamond_producer.c)
target_link_libraries(test_xir_mixed_diamond_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_diamond_producer PRIVATE
    XR_DIAMOND_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_diamond_init")
set(XIR_DIAMOND_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_diamond.c")
add_custom_command(OUTPUT "${XIR_DIAMOND_GENERATED}"
    COMMAND $<TARGET_FILE:test_xir_mixed_diamond_producer> "${XIR_DIAMOND_GENERATED}"
    DEPENDS test_xir_mixed_diamond_producer xir/fixtures/p20_diamond_init/root.xr xir/fixtures/p20_diamond_init/left.xr
        xir/fixtures/p20_diamond_init/right.xr xir/fixtures/p20_diamond_init/shared.xr VERBATIM)
add_executable(test_xir_mixed_diamond xir/test_xir_mixed_diamond.c "${XIR_DIAMOND_GENERATED}")
target_link_libraries(test_xir_mixed_diamond PRIVATE xray_xir_vm xray_xir_scalar)
add_executable(test_xir_mixed_diamond_resources xir/test_xir_mixed_diamond_resources.c "${XIR_DIAMOND_GENERATED}")
target_link_libraries(test_xir_mixed_diamond_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(diamond_target IN ITEMS test_xir_mixed_diamond_producer test_xir_mixed_diamond test_xir_mixed_diamond_resources)
    set_target_properties(${diamond_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${diamond_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${diamond_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(diamond_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_diamond_${diamond_backend} COMMAND test_xir_mixed_diamond ${diamond_backend})
    set_tests_properties(test_xir_mixed_diamond_${diamond_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;diamond;initialization")
    foreach(diamond_gate IN ITEMS axes fi)
        add_test(NAME test_xir_mixed_diamond_${diamond_backend}_${diamond_gate}
            COMMAND test_xir_mixed_diamond_resources ${diamond_backend} ${diamond_gate})
        set_tests_properties(test_xir_mixed_diamond_${diamond_backend}_${diamond_gate} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
            LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;diamond;initialization;${diamond_gate}")
    endforeach()
endforeach()
