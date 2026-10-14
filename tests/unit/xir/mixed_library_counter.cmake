add_executable(test_xir_mixed_library_producer xir/test_xir_mixed_library_producer.c)
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
add_executable(test_xir_mixed_library_counter xir/test_xir_mixed_library_counter.c "${XIR_LIBRARY_COUNTER_GENERATED}")
target_link_libraries(test_xir_mixed_library_counter PRIVATE xray_xir_vm xray_xir_scalar)
foreach(counter_target IN ITEMS test_xir_mixed_library_producer test_xir_mixed_library_counter)
    set_target_properties(${counter_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${counter_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${counter_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(counter_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_library_counter_${counter_backend} COMMAND test_xir_mixed_library_counter ${counter_backend})
    set_tests_properties(test_xir_mixed_library_counter_${counter_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;library;module-state")
endforeach()
