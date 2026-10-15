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
add_executable(test_xir_mixed_library_resources xir/test_xir_mixed_library_resources.c "${XIR_LIBRARY_COUNTER_GENERATED}")
target_link_libraries(test_xir_mixed_library_resources PRIVATE xray_xir_vm xray_xir_scalar)
foreach(counter_target IN ITEMS test_xir_mixed_library_producer test_xir_mixed_library_counter test_xir_mixed_library_resources)
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
add_executable(test_xir_mixed_library_pure_worker xir/test_xir_mixed_library_pure_worker.c "${library_pure_source}")
target_link_libraries(test_xir_mixed_library_pure_worker PRIVATE xray_xir_vm xray_xir_scalar)
foreach(library_pure_target IN ITEMS test_xir_mixed_library_pure_producer test_xir_mixed_library_pure_worker)
    set_target_properties(${library_pure_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${library_pure_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${library_pure_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(library_pure_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_library_pure_worker_${library_pure_backend} COMMAND test_xir_mixed_library_pure_worker ${library_pure_backend})
    set_tests_properties(test_xir_mixed_library_pure_worker_${library_pure_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;root;worker;generated-c;lifecycle;library;module-state")
endforeach()
add_executable(test_xir_mixed_library_pure_permissions xir/test_xir_mixed_library_pure_permissions.c)
target_link_libraries(test_xir_mixed_library_pure_permissions PRIVATE xray_xir_source xray_xir_cgen)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-permissions-absent")
target_compile_definitions(test_xir_mixed_library_pure_permissions PRIVATE
    XR_COUNTER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/unit/xir/fixtures/p20_library_pure_worker"
    XR_COUNTER_LOGICAL_ROOT="${CMAKE_CURRENT_BINARY_DIR}/generated/p20-library-permissions-absent")
set_target_properties(test_xir_mixed_library_pure_permissions PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_library_pure_permissions PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_library_pure_permissions PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_mixed_library_pure_permissions COMMAND test_xir_mixed_library_pure_permissions)
set_tests_properties(test_xir_mixed_library_pure_permissions PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
    LABELS "unit;xir;mixed;source;ownership;root;worker;library;permissions")
foreach(counter_backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_library_counter_${counter_backend} COMMAND test_xir_mixed_library_counter ${counter_backend})
    set_tests_properties(test_xir_mixed_library_counter_${counter_backend} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
        LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;library;module-state")
    foreach(counter_resource_mode IN ITEMS axes fi)
        add_test(NAME test_xir_mixed_library_${counter_backend}_${counter_resource_mode}
            COMMAND test_xir_mixed_library_resources ${counter_backend} ${counter_resource_mode})
        set_tests_properties(test_xir_mixed_library_${counter_backend}_${counter_resource_mode} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE COST 1
            LABELS "unit;xir;mixed;execution;ownership;instance;generated-c;lifecycle;library;module-state;${counter_resource_mode}")
    endforeach()
endforeach()
