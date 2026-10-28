# Direct storage tests compile the same owner used by the legacy VM.
add_executable(test_vm_stack_growth
    test_vm_stack_growth.c
    stack_growth_faults.c
    ${PROJECT_SOURCE_DIR}/src/vm/xvm_stack.c)
target_include_directories(test_vm_stack_growth PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_vm_stack_growth PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
target_compile_options(test_vm_stack_growth PRIVATE
    $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
set_property(TARGET test_vm_stack_growth PROPERTY MSVC_RUNTIME_LIBRARY
    $<TARGET_GENEX_EVAL:xray_core_objs,$<TARGET_PROPERTY:xray_core_objs,MSVC_RUNTIME_LIBRARY>>)
set_target_properties(test_vm_stack_growth PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
# This legacy context includes pre-existing flexible-array and conversion
# diagnostics. Inherit its canonical warning policy without suppressions.
if(MSVC)
    target_compile_options(test_vm_stack_growth PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME test_vm_stack_growth COMMAND test_vm_stack_growth)
set_tests_properties(test_vm_stack_growth PROPERTIES
    LABELS "unit;vm;ownership;fault-injection" TIMEOUT 30)
